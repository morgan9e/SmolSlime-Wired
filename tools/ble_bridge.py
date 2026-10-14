#!/usr/bin/env python3
"""Trackers in BLE mode (tracker console: ble on) to the stock SlimeVR server over UDP. No dongle.

    python ble_bridge.py                       # every SlimeNRF tracker it finds
    python ble_bridge.py --only 0791,B8C8      # only SlimeNRF-0791 and SlimeNRF-B8C8
    python ble_bridge.py --server 192.168.1.5  # SlimeVR server on another PC
    python ble_bridge.py --serial              # through an ESP32-C6 BLE receiver (BleReceiver/)

With --serial the trackers connect to an ESP32-C6 running BleReceiver/ instead of this PC's
Bluetooth, and it passes them on over USB (pyserial, no bleak). Port found by USB id, or
--serial COM23.

Each tracker advertises as SlimeNRF-XXXX (the last 4 hex digits of its device address, as `info`
on its console shows it) and notifies the same 16-byte packets it sends over ESB, plus a sequence
number (Tracker/src/connection/ble.c). The bridge reads the tracker's device address and
registers it with SlimeVR under that address, as tools/quest_bridge.py does. Start the SlimeVR
server first; Ctrl+C stops.

Settings over the radio while it runs: remote_config.py --ble 0791 get|set|reset|list ... goes
through this bridge (its control port, 127.0.0.1:6971), since a tracker takes one BLE connection.
Back to ESB: remote_config.py --ble 0791 set connection_over_ble 0 (the tracker reboots), or
`ble off` on its USB console. Firmware updates over the radio (ota.py) need ESB.

Needs bleak: python -m pip install bleak
"""
import argparse
import asyncio
import importlib
import logging
import time

import quest_bridge

LOG = logging.getLogger("ble")

# Tracker/src/connection/ble.c SLIME_UUID_VAL
SERVICE_UUID = "6e5c0001-7a3b-4c8e-9f1d-2b4a5c6d7e8f"
DATA_UUID = "6e5c0002-7a3b-4c8e-9f1d-2b4a5c6d7e8f"
ADDRESS_UUID = "6e5c0003-7a3b-4c8e-9f1d-2b4a5c6d7e8f"
CONFIG_UUID = "6e5c0004-7a3b-4c8e-9f1d-2b4a5c6d7e8f"  # settings: write CFG_REQ, notify CFG_RESP
NAME_PREFIX = "SlimeNRF-"

# Control port (remote_config.py --ble): a datagram is CONTROL_MAGIC, the tracker's name without
# SlimeNRF- (8 bytes, NUL padded) and a 10-byte settings message; the bridge answers in the same
# form with the tracker's answer, or with no message if that tracker is not connected.
CONTROL_PORT = 6971
CONTROL_MAGIC = b"SNCF"
CFG_MSG_LEN = 10


def control_pack(name, message=b""):
    return CONTROL_MAGIC + name.upper().encode("ascii").ljust(8, b"\0")[:8] + bytes(message)


def control_unpack(data):
    """(name, message); message is b"" when the bridge has no connection to that tracker."""
    data = bytes(data)
    if len(data) < 12 or data[:4] != CONTROL_MAGIC or len(data) - 12 not in (0, CFG_MSG_LEN):
        raise ValueError("not a control message")
    return data[4:12].rstrip(b"\0").decode("ascii", "replace").upper(), data[12:]


def split(data):
    """A data notification: the 16-byte packet and its sequence number."""
    data = bytes(data)
    if len(data) != 17 or data[0] not in range(8):
        raise ValueError("not a tracker packet")
    return data[:16], data[16]


def tracker_mac(address):
    """The address characteristic (device address, little endian) as SlimeVR's 6-byte MAC."""
    address = bytes(address)
    if len(address) != 6:
        raise ValueError("address must be 6 bytes")
    return address[::-1]


class Link:
    """One tracker: its notifications to a quest_bridge.Device (one SlimeVR UDP session)."""

    def __init__(self, mac, server, factory=quest_bridge.Device):
        self.mac, self.server, self.factory = mac, server, factory
        self.device = None
        self.sequence = None
        self.packets = self.lost = self.rejected = 0

    def receive(self, data, now):
        try:
            p, seq = split(data)
        except ValueError:
            self.rejected += 1
            return
        if self.sequence is not None:
            delta = (seq - self.sequence) & 0xff
            if delta == 0:
                return  # repeated
            if delta < 128:
                self.lost += delta - 1
        self.sequence = seq
        self.packets += 1
        if self.device is None:
            if p[0] != 0:
                return  # SlimeVR's handshake needs the device info packet (board, IMU)
            LOG.info("Tracker %s", self.mac.hex(":").upper())
            self.device = self.factory(self.mac, p, self.server)
        self.device.record(p, now)

    def disconnected(self):
        self.sequence = None  # the next connection starts its own count

    def tick(self, now):
        if self.device:
            self.device.tick(now)

    def close(self):
        if self.device:
            self.device.sock.close()


def wanted(name, only):
    """only: name suffixes (the hex digits after SlimeNRF-, any case); empty takes every tracker."""
    if not name or not name.startswith(NAME_PREFIX):
        return False
    return not only or name[len(NAME_PREFIX):].upper() in only


class Bridge:
    def __init__(self, server, only=()):
        self.server, self.only = server, {o.strip().upper() for o in only if o.strip()}
        self.links = {}  # by tracker MAC, kept across reconnections
        self.busy = set()  # BLE addresses being connected or connected
        self.connect_lock = asyncio.Lock()  # Windows connects one device at a time more reliably
        self.clients = {}  # name without SlimeNRF- -> BleakClient with settings, while connected
        self.config_peers = {}  # name -> control port address waiting for that tracker's answer
        self.control = None  # the control port's transport
        self.linked = set()  # names without SlimeNRF- of the connected trackers
        self.scanner = None
        self.scanning = False

    # Windows connects unreliably while it scans (connections time out): scanning pauses for each
    # connection attempt, and stays off once every --only tracker is connected.
    async def set_scanning(self, on):
        if on and self.only and self.only <= self.linked:
            on = False
        if self.scanner and on != self.scanning:
            await (self.scanner.start() if on else self.scanner.stop())
            self.scanning = on

    async def run_tracker(self, device, name):
        from bleak import BleakClient
        short = name[len(NAME_PREFIX):].upper()
        gone = asyncio.Event()
        link = None
        try:
            async with self.connect_lock:
                await self.set_scanning(False)
                try:
                    client = BleakClient(device, disconnected_callback=lambda c: gone.set())
                    await client.connect(timeout=15)
                    self.linked.add(short)
                finally:
                    await self.set_scanning(True)
            try:
                mac = tracker_mac(await client.read_gatt_char(ADDRESS_UUID))
                link = self.links.get(mac) or self.links.setdefault(mac, Link(mac, self.server))
                await client.start_notify(DATA_UUID, lambda _c, data: link.receive(data, time.monotonic()))
                try:
                    await client.start_notify(CONFIG_UUID, lambda _c, data: self.config_answer(short, data))
                    self.clients[short] = client
                except Exception as e:
                    LOG.warning("%s: no settings over BLE (older firmware?): %s", name, e)
                LOG.info("Connected %s (%s)", name, device.address)
                await gone.wait()
            finally:
                self.clients.pop(short, None)
                await client.disconnect()
        except Exception as e:  # BLE errors vary by platform backend
            LOG.warning("%s: %s", name, e)
        finally:
            if link:
                link.disconnected()
            LOG.info("Disconnected %s", name)
            self.busy.discard(device.address)
            self.linked.discard(short)
            await self.set_scanning(True)  # find it again

    def found(self, device, adv):
        name = adv.local_name or device.name
        if device.address in self.busy or not wanted(name, self.only):
            return
        self.busy.add(device.address)
        asyncio.get_running_loop().create_task(self.run_tracker(device, name))

    # Settings over BLE for remote_config.py --ble, through the control port
    def config_request(self, data, peer):
        try:
            name, message = control_unpack(data)
        except ValueError:
            return
        client = self.clients.get(name)
        if not client or not message:
            self.control.sendto(control_pack(name), peer)  # not connected
            return
        self.config_peers[name] = peer
        asyncio.get_running_loop().create_task(self.config_write(name, client, message, peer))

    async def config_write(self, name, client, message, peer):
        try:
            await client.write_gatt_char(CONFIG_UUID, message, response=True)
        except Exception as e:
            LOG.warning("SlimeNRF-%s settings: %s", name, e)
            self.control.sendto(control_pack(name), peer)

    def config_answer(self, name, data):
        peer = self.config_peers.get(name)
        if peer and self.control and len(data) == CFG_MSG_LEN:
            self.control.sendto(control_pack(name, data), peer)

    def tick(self, now):
        for link in self.links.values():
            link.tick(now)

    def close(self):
        for link in self.links.values():
            link.close()


class ControlPort(asyncio.DatagramProtocol):
    def __init__(self, bridge):
        self.bridge = bridge

    def connection_made(self, transport):
        self.bridge.control = transport

    def datagram_received(self, data, addr):
        self.bridge.config_request(data, addr)


RECEIVER_USB = (0x303A, 0x1001)  # ESP32-C6 USB Serial/JTAG


class SerialSlot:
    """A tracker on the ESP32-C6 receiver, as the settings path (Bridge.config_write) uses it."""

    def __init__(self, receiver, slot):
        self.receiver, self.slot = receiver, slot

    async def write_gatt_char(self, uuid, message, response=True):
        self.receiver.write(f"W {self.slot} {bytes(message).hex().upper()}")


class SerialReceiver:
    """BleReceiver/ (ESP32-C6) on its USB serial port: its text lines (BleReceiver/main/main.c)
    to the Bridge's Links, settings requests back to it."""

    def __init__(self, bridge, write):
        self.bridge, self.write = bridge, write
        self.slots = {}  # receiver slot -> (name without SlimeNRF-, Link)

    def line(self, text, now):
        parts = text.split()
        if not parts:
            return
        kind, args = parts[0], parts[1:]
        try:
            if kind == "D" and len(args) == 2:
                if int(args[0]) in self.slots:
                    self.slots[int(args[0])][1].receive(bytes.fromhex(args[1]), now)
            elif kind == "C" and len(args) == 2:
                slot, mac = int(args[0]), bytes.fromhex(args[1])
                if len(mac) != 6:
                    return
                short = args[1][-4:].upper()
                link = self.bridge.links.get(mac) or self.bridge.links.setdefault(mac, Link(mac, self.bridge.server))
                self.slots[slot] = (short, link)
                self.bridge.clients[short] = SerialSlot(self, slot)
                self.bridge.linked.add(short)
                LOG.info("Connected SlimeNRF-%s (receiver slot %d)", short, slot)
            elif kind == "X" and args:
                gone = self.slots.pop(int(args[0]), None)
                if gone:
                    short, link = gone
                    link.disconnected()
                    self.bridge.clients.pop(short, None)
                    self.bridge.linked.discard(short)
                    LOG.info("Disconnected SlimeNRF-%s (0x%s)", short, args[1] if len(args) > 1 else "?")
            elif kind == "F" and len(args) == 2:
                if int(args[0]) in self.slots:
                    self.bridge.config_answer(self.slots[int(args[0])][0], bytes.fromhex(args[1]))
            elif kind in ("E", "I", "A"):
                LOG.info("Receiver: %s", text)
            else:
                LOG.debug("Receiver: %s", text)
        except ValueError:
            LOG.debug("Receiver line not understood: %s", text)


def find_receiver_port():
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if (p.vid, p.pid) == RECEIVER_USB]
    if len(ports) != 1:
        raise SystemExit(f"--serial: {'no' if not ports else 'more than one'} ESP32-C6 receiver found "
                         f"{ports or ''}; name its port, e.g. --serial COM23")
    return ports[0]


def open_receiver(port):
    """The ESP32-C6's USB Serial/JTAG resets the chip on some DTR/RTS changes (pyserial asserts
    both on open, Windows drops DTR first on close): both stay off, so it keeps its connections."""
    import serial
    ser = serial.Serial(timeout=0.2)
    ser.port, ser.dtr, ser.rts = port, False, False
    ser.open()
    return ser


async def report_loop(bridge):
    """Every 5 s per tracker: rotations/s sent to SlimeVR, packets/s received, packets lost."""
    report_at = time.monotonic()
    counts = {}
    while True:
        await asyncio.sleep(0.02)
        now = time.monotonic()
        bridge.tick(now)
        if now - report_at >= 5:
            parts = []
            for mac, link in bridge.links.items():
                before = counts.get(mac, (0, 0))
                rate = (link.packets - before[0]) / (now - report_at)
                rotations = link.device.rotations if link.device else 0
                lost = link.lost - before[1]
                parts.append(f"{mac.hex()[-4:].upper()}:{rotations / (now - report_at):.0f}Hz"
                             f"/{rate:.0f}pk lost {lost}"
                             + ("" if link.device and link.device.connected else " (no server)"))
                counts[mac] = (link.packets, link.lost)
                if link.device:
                    link.device.rotations = 0
            LOG.info("%d linked; %s", len(bridge.linked), " ".join(parts) or "no trackers yet")
            report_at = now


async def start_control(bridge, port):
    # Local only: settings requests are not authenticated
    control, _ = await asyncio.get_running_loop().create_datagram_endpoint(
        lambda: ControlPort(bridge), local_addr=("127.0.0.1", port))
    return control


async def main_async(args):
    from bleak import BleakScanner
    bridge = Bridge((args.server, args.server_port), args.only.split(",") if args.only else ())
    control = await start_control(bridge, args.control_port)
    scanner = bridge.scanner = BleakScanner(bridge.found, service_uuids=[SERVICE_UUID])
    await bridge.set_scanning(True)
    LOG.info("Scanning for SlimeNRF trackers; SlimeVR %s:%d; settings on 127.0.0.1:%d",
             args.server, args.server_port, args.control_port)
    try:
        await report_loop(bridge)
    finally:
        await bridge.set_scanning(False)
        control.close()
        bridge.close()


async def main_serial(args):
    import threading
    import serial
    port = find_receiver_port() if args.serial == "auto" else args.serial
    ser = open_receiver(port)
    loop = asyncio.get_running_loop()
    bridge = Bridge((args.server, args.server_port))
    write_lock = threading.Lock()

    def write(text):
        with write_lock:
            ser.write((text + "\n").encode("ascii"))

    receiver = SerialReceiver(bridge, write)
    stop = threading.Event()
    failed = loop.create_future()

    def read():  # pyserial blocks: its own thread, whole lines to the event loop
        buf = b""
        while not stop.is_set():
            try:
                buf += ser.read(4096)
            except serial.SerialException as e:
                loop.call_soon_threadsafe(lambda: failed.done() or failed.set_exception(e))
                return
            *lines, buf = buf.split(b"\n")
            now = time.monotonic()
            for raw in lines:
                loop.call_soon_threadsafe(receiver.line, raw.decode("ascii", "replace").strip(), now)

    threading.Thread(target=read, daemon=True).start()
    write("O " + (args.only or ""))  # which trackers it connects
    write("?")  # and the ones it has already
    control = await start_control(bridge, args.control_port)
    LOG.info("ESP32-C6 receiver on %s; SlimeVR %s:%d; settings on 127.0.0.1:%d",
             port, args.server, args.server_port, args.control_port)
    report = asyncio.ensure_future(report_loop(bridge))
    try:
        await asyncio.wait([report, failed], return_when=asyncio.FIRST_COMPLETED)
        failed.done() and failed.result()  # raises the serial error
    finally:
        report.cancel()
        stop.set()
        control.close()
        bridge.close()
        ser.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", default="127.0.0.1")
    parser.add_argument("--server-port", type=int, default=6969)
    parser.add_argument("--only", help="Comma-separated tracker names without SlimeNRF-, e.g. 0791,B8C8")
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT,
                        help="localhost UDP port for remote_config.py --ble")
    parser.add_argument("--serial", nargs="?", const="auto", metavar="PORT",
                        help="through an ESP32-C6 BLE receiver (BleReceiver/) on this port; found by its USB id if left out")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(message)s")
    try:
        importlib.import_module("serial" if args.serial else "bleak")
    except ImportError:
        raise SystemExit("Needs pyserial: python -m pip install pyserial" if args.serial else
                         "Needs bleak: python -m pip install bleak")
    try:
        asyncio.run(main_serial(args) if args.serial else main_async(args))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
