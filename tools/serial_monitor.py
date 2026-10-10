"""A small desktop serial monitor. Install pyserial before running."""

import codecs
import json
import os
import queue
import subprocess
import threading
import time
import tkinter as tk
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("Missing dependency. Run: python -m pip install pyserial")


SETTINGS = Path(__file__).with_name("serial_monitor_settings.json")


def identity(port, ports=()):
    result = {key: getattr(port, key, None) for key in
              ("device", "serial_number", "vid", "pid", "interface")}
    result["multiple_interfaces"] = sum(
        (p.serial_number, p.vid, p.pid) == (port.serial_number, port.vid, port.pid)
        for p in ports) > 1
    return result


def find_target(ports, target):
    if not target:
        return None
    if target.get("serial_number"):
        candidates = [p for p in ports if all(getattr(p, key, None) == target.get(key)
                      for key in ("serial_number", "vid", "pid", "interface"))]
        # Composite devices may expose several serial interfaces. Never guess.
        if target.get("multiple_interfaces") and not target.get("interface"):
            return next((p for p in candidates if p.device == target.get("device")), None)
        return candidates[0] if len(candidates) == 1 else next(
            (p for p in candidates if p.device == target.get("device")), None)
    return next((p for p in ports if p.device == target.get("device") and
                 all(target.get(key) is None or getattr(p, key, None) == target[key]
                     for key in ("vid", "pid"))), None)


def windows_board_names():
    """Windows' bus-reported product name is often richer than its port label."""
    script = r'''
    $ErrorActionPreference = 'Stop'
    @(Get-PnpDevice -Class Ports -PresentOnly | Where-Object InstanceId -Like 'USB\*' | ForEach-Object {
        $portDevice = $_
        $bus = Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_BusReportedDeviceDesc' -ErrorAction SilentlyContinue
        if (-not $bus.Data) {
            $parentDevice = Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Parent' -ErrorAction SilentlyContinue
            if ($parentDevice.Data) {
                $bus = Get-PnpDeviceProperty -InstanceId $parentDevice.Data -KeyName 'DEVPKEY_Device_BusReportedDeviceDesc' -ErrorAction SilentlyContinue
            }
        }
        if ($portDevice.FriendlyName -match '\((COM\d+)\)' -and $bus.Data) {
            [pscustomobject]@{port=$Matches[1]; name=[string]$bus.Data}
        }
    }) | ConvertTo-Json -Compress
    '''
    try:
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
             "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new();" + script],
            capture_output=True, encoding="utf-8", errors="replace", timeout=12,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), check=True)
        rows = json.loads(result.stdout or "[]")
        if isinstance(rows, dict):
            rows = [rows]
        return {row["port"]: row["name"] for row in rows}
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError):
        return {}


def discover(events, cache):
    try:
        ports = sorted(list_ports.comports())
        keys = [(p.device, p.hwid) for p in ports]
        names = windows_board_names() if os.name == "nt" and any(k not in cache for k in keys) else {}
        for port, key in zip(ports, keys):
            cache.setdefault(key, names.get(port.device) or port.product or port.description or "Unknown device")
            port.board_name = cache[key]
        events.put(("ports", ports))
    except (OSError, serial.SerialException) as error:
        events.put(("scan_error", str(error)))


def serial_worker(port, baud, outgoing, events, stop):
    """Keep all potentially blocking device access off the GUI thread."""
    device = None
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    try:
        device = serial.Serial(port, baudrate=baud, timeout=0.1, write_timeout=1)
        events.put(("connected", f"Connected to {port} at {baud} baud"))
        while not stop.is_set():
            try:
                payload = outgoing.get_nowait()
            except queue.Empty:
                payload = None
            if payload is not None:
                count = device.write(payload)
                if count != len(payload):
                    raise serial.SerialTimeoutException("Only part of the message was sent")
                events.put(("sent", payload))
            data = device.read(min(max(device.in_waiting, 1), 8192))
            if data:
                text = decoder.decode(data)
                if text:
                    events.put(("received", text))
    except (serial.SerialException, OSError, ValueError) as error:
        events.put(("error", str(error)))
    finally:
        tail = decoder.decode(b"", final=True)
        if tail:
            events.put(("received", tail))
        if device is not None:
            try:
                device.close()
            except (serial.SerialException, OSError):
                pass
        events.put(("closed", None))


class SerialMonitor:
    ENDINGS = {"None": "", "LF (\\n)": "\n", "CR (\\r)": "\r", "CRLF (\\r\\n)": "\r\n"}

    def __init__(self, root):
        self.root = root
        self.worker = None
        self.stop = threading.Event()
        self.events = queue.Queue()
        self.outgoing = queue.Queue(maxsize=100)
        self.connected = False
        self.closing = False
        self.target = None
        self.was_connected = False
        self.manual_disconnect = False
        self.next_attempt = 0
        self.retry_delay = 3
        self.scanning = False
        self.next_scan = 0
        self.name_cache = {}
        self.port_info = []
        self.port = tk.StringVar()
        self.port_devices = {}
        self.baud = tk.StringVar(value="115200")
        self.ending = tk.StringVar(value="LF (\\n)")
        self.message = tk.StringVar()
        self.input_history = []
        self.history_index = 0
        self.history_draft = ""
        self.status = tk.StringVar(value="Disconnected")
        self.autoscroll = tk.BooleanVar(value=True)
        self.auto_connect = tk.BooleanVar(value=False)
        self.auto_reconnect = tk.BooleanVar(value=True)
        self.device_details = tk.StringVar(value="Looking for devices…")
        try:
            settings = json.loads(SETTINGS.read_text(encoding="utf-8"))
            if not isinstance(settings, dict):
                raise ValueError("Invalid settings")
            self.port.set(str(settings.get("port", "")))
            self.baud.set(str(settings.get("baud", "115200")))
            self.auto_connect.set(settings.get("auto_connect") is True)
            self.auto_reconnect.set(settings.get("auto_reconnect", True) is True)
            if isinstance(settings.get("target"), dict):
                self.target = settings["target"]
        except (OSError, ValueError, TypeError):
            pass
        root.title("Serial Monitor")
        root.geometry("880x560")
        root.minsize(820, 400)
        root.protocol("WM_DELETE_WINDOW", self.close)

        frame = ttk.Frame(root, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(1, weight=1)
        toolbar = ttk.Frame(frame)
        toolbar.grid(row=0, column=0, sticky="ew", pady=(0, 10))
        ttk.Label(toolbar, text="Port").pack(side="left")
        self.ports = ttk.Combobox(toolbar, textvariable=self.port, width=38)
        self.ports.pack(side="left", padx=6)
        self.ports.bind("<<ComboboxSelected>>", self.selection_changed)
        self.ports.bind("<FocusOut>", self.selection_changed)
        self.refresh_button = ttk.Button(toolbar, text="Refresh", command=self.refresh)
        self.refresh_button.pack(side="left")
        ttk.Label(toolbar, text="Baud").pack(side="left", padx=(18, 0))
        self.bauds = ttk.Combobox(toolbar, textvariable=self.baud, width=10,
                                 values=(1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600))
        self.bauds.pack(side="left", padx=6)
        self.connect_button = ttk.Button(toolbar, text="Connect", command=self.toggle)
        self.connect_button.pack(side="left", padx=6)

        self.log = ScrolledText(frame, wrap="word", font=("Consolas", 10),
                                background="#15202b", foreground="#e4edf5",
                                insertbackground="white", state="disabled")
        self.log.grid(row=1, column=0, sticky="nsew")
        self.log.tag_configure("info", foreground="#8cb9d9")
        self.log.tag_configure("sent", foreground="#8cdbad")
        self.log.tag_configure("error", foreground="#ff9d9d")

        controls = ttk.Frame(frame)
        controls.grid(row=2, column=0, sticky="ew", pady=8)
        ttk.Checkbutton(controls, text="Auto-scroll", variable=self.autoscroll).pack(side="left")
        ttk.Checkbutton(controls, text="Auto-connect", variable=self.auto_connect,
                        command=self.auto_changed).pack(side="left", padx=8)
        ttk.Checkbutton(controls, text="Auto-reconnect", variable=self.auto_reconnect,
                        command=self.auto_changed).pack(side="left")
        ttk.Button(controls, text="Clear", command=self.clear).pack(side="right")
        ttk.Button(controls, text="Save log…", command=self.save).pack(side="right", padx=6)
        sendbar = ttk.Frame(frame)
        sendbar.grid(row=3, column=0, sticky="ew")
        sendbar.columnconfigure(0, weight=1)
        self.entry = ttk.Entry(sendbar, textvariable=self.message)
        self.entry.grid(row=0, column=0, sticky="ew", padx=(0, 8))
        self.entry.bind("<Return>", self.send)
        self.entry.bind("<Up>", lambda event: self.browse_history(-1))
        self.entry.bind("<Down>", lambda event: self.browse_history(1))
        ttk.Combobox(sendbar, textvariable=self.ending, values=list(self.ENDINGS),
                     width=14, state="readonly").grid(row=0, column=1, padx=(0, 8))
        self.send_button = ttk.Button(sendbar, text="Send", command=self.send, state="disabled")
        self.send_button.grid(row=0, column=2)
        ttk.Label(frame, textvariable=self.status).grid(row=4, column=0, sticky="w", pady=(10, 0))
        ttk.Label(frame, textvariable=self.device_details, wraplength=780).grid(
            row=5, column=0, sticky="w", pady=(5, 0))
        self.refresh()
        self.root.after(50, self.poll)

    def refresh(self):
        if not self.scanning and not self.closing:
            self.scanning = True
            self.next_scan = time.monotonic() + 3
            threading.Thread(target=discover, args=(self.events, self.name_cache), daemon=True).start()

    def update_ports(self, ports):
        selected = self.selected_port()
        self.port_info = ports
        target_port = find_target(ports, self.target)
        if self.worker is None and target_port:
            selected = target_port.device
        self.port_devices = {f"{p.device} — {p.board_name}": p.device for p in ports}
        self.ports["values"] = list(self.port_devices)
        label = next((label for label, device in self.port_devices.items() if device == selected), None)
        # Leave selection empty on first launch: do not open an arbitrary device.
        self.port.set(label or selected)
        self.show_device()

    def show_device(self):
        port = next((p for p in self.port_info if p.device == self.selected_port()), None)
        if port is None:
            self.device_details.set("Selected device is unavailable." if self.selected_port() else "Select a device to connect.")
            return
        details = [f"Board: {port.board_name}"]
        if port.manufacturer:
            details.append(f"Manufacturer: {port.manufacturer}")
        if port.serial_number:
            details.append(f"Serial: {port.serial_number}")
        if port.vid is not None and port.pid is not None:
            details.append(f"USB: {port.vid:04X}:{port.pid:04X}")
        self.device_details.set("  •  ".join(details))

    def selection_changed(self, event=None):
        if self.worker is not None:
            return
        selected = self.selected_port()
        if self.target and selected == self.target.get("device"):
            return
        port = next((p for p in self.port_info if p.device == selected), None)
        self.target = identity(port, self.port_info) if port else {"device": selected}
        self.was_connected = False
        self.manual_disconnect = False
        self.next_attempt = 0
        self.show_device()

    def auto_changed(self):
        self.manual_disconnect = False
        self.next_attempt = 0
        self.retry_delay = 3

    def automatic_connect(self):
        if self.worker is not None or self.closing or self.manual_disconnect:
            return
        enabled = self.auto_reconnect.get() if self.was_connected else self.auto_connect.get()
        if not enabled or not self.selected_port():
            return
        if not self.target:
            self.selection_changed()
        match = find_target(self.port_info, self.target)
        if match is None:
            self.status.set("Waiting for selected device…")
        elif time.monotonic() >= self.next_attempt:
            self.open_connection(automatic=True)

    def selected_port(self):
        value = self.port.get().strip()
        return self.port_devices.get(value, value)

    def append(self, text, tag=None):
        self.log.configure(state="normal")
        self.log.insert("end", text, (tag,) if tag else ())
        # Keep long-running sessions from growing the displayed log indefinitely.
        chars = self.log.count("1.0", "end-1c", "chars")[0]
        if chars > 1_000_000:
            self.log.delete("1.0", f"1.0+{chars - 800_000}c")
        self.log.configure(state="disabled")
        if self.autoscroll.get():
            self.log.see("end")

    def note(self, text, tag="info"):
        self.append(f"\n[{datetime.now():%H:%M:%S}] {text}\n", tag)

    def toggle(self):
        if self.worker is not None:
            self.manual_disconnect = True
            self.stop.set()
            self.connected = False
            self.send_button.configure(state="disabled")
            self.connect_button.configure(state="disabled")
            self.status.set("Disconnecting…")
            return
        self.manual_disconnect = False
        self.open_connection()

    def open_connection(self, automatic=False):
        port = self.selected_port()
        try:
            baud = int(self.baud.get())
            if not port or baud <= 0:
                raise ValueError
        except ValueError:
            self.status.set("Enter a port and a positive whole-number baud rate.")
            if not automatic:
                messagebox.showerror("Connection settings", self.status.get())
            self.next_attempt = time.monotonic() + 3
            return
        info = next((p for p in self.port_info if p.device == port), None)
        self.target = identity(info, self.port_info) if info else {"device": port}
        self.stop = threading.Event()
        self.outgoing = queue.Queue(maxsize=100)
        for widget in (self.ports, self.bauds, self.refresh_button):
            widget.configure(state="disabled")
        self.connect_button.configure(text="Disconnect")
        self.status.set(f"Opening {port}…")
        self.worker = threading.Thread(target=serial_worker,
                                       args=(port, baud, self.outgoing, self.events, self.stop), daemon=True)
        self.worker.start()

    def browse_history(self, direction):
        if not self.input_history:
            return "break"
        if self.history_index == len(self.input_history):
            self.history_draft = self.message.get()
        self.history_index = max(0, min(len(self.input_history), self.history_index + direction))
        self.message.set(self.history_draft if self.history_index == len(self.input_history)
                         else self.input_history[self.history_index])
        self.entry.selection_clear()
        self.entry.icursor("end")
        return "break"

    def send(self, event=None):
        if not self.connected:
            return
        message = self.message.get()
        payload = (message + self.ENDINGS[self.ending.get()]).encode("utf-8")
        if not payload:
            return
        try:
            self.outgoing.put_nowait(payload)
            if message and (not self.input_history or message != self.input_history[-1]):
                self.input_history.append(message)
                self.input_history = self.input_history[-100:]
            self.history_index = len(self.input_history)
            self.history_draft = ""
            self.message.set("")
        except queue.Full:
            messagebox.showwarning("Send queue full", "Wait for pending messages to finish sending.")

    def poll(self):
        for _ in range(200):
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "connected":
                if not self.stop.is_set():
                    self.connected = True
                    self.was_connected = True
                    self.retry_delay = 3
                    self.status.set(value + " • 8N1 • UTF-8")
                    self.send_button.configure(state="normal")
                self.note(value)
            elif kind == "received":
                self.append(value)
            elif kind == "sent":
                self.note("TX " + repr(value.decode("utf-8")), "sent")
            elif kind == "error":
                self.note(value, "error")
            elif kind == "closed":
                self.worker = None
                self.connected = False
                self.send_button.configure(state="disabled")
                self.connect_button.configure(text="Connect", state="normal")
                for widget in (self.ports, self.bauds, self.refresh_button):
                    widget.configure(state="normal")
                self.status.set("Disconnected")
                self.note("Disconnected")
                self.next_attempt = time.monotonic() + self.retry_delay
                self.retry_delay = min(self.retry_delay * 2, 30)
            elif kind == "ports":
                self.scanning = False
                self.update_ports(value)
            elif kind == "scan_error":
                self.scanning = False
                if self.worker is None:
                    self.status.set(f"Device discovery failed: {value}")
        if self.closing and self.worker is None:
            self.root.destroy()
            return
        if time.monotonic() >= self.next_scan:
            self.refresh()
        self.automatic_connect()
        self.root.after(50, self.poll)

    def clear(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def save(self):
        path = filedialog.asksaveasfilename(defaultextension=".txt",
                                          filetypes=[("Text log", "*.txt"), ("All files", "*.*")])
        if path:
            try:
                with open(path, "w", encoding="utf-8") as stream:
                    stream.write(self.log.get("1.0", "end-1c"))
            except OSError as error:
                messagebox.showerror("Save failed", str(error))

    def close(self):
        settings = {"port": self.selected_port(), "baud": self.baud.get(),
                    "target": self.target, "auto_connect": self.auto_connect.get(),
                    "auto_reconnect": self.auto_reconnect.get()}
        try:
            SETTINGS.write_text(json.dumps(settings, indent=2), encoding="utf-8")
        except OSError as error:
            messagebox.showwarning("Settings not saved", str(error))
        self.closing = True
        self.stop.set()
        self.connected = False
        self.send_button.configure(state="disabled")
        self.connect_button.configure(state="disabled")
        self.status.set("Closing…")


if __name__ == "__main__":
    SerialMonitor(tk.Tk()).root.mainloop()
