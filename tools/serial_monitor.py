"""Serial Monitor: a tabbed desktop serial console for the receiver and trackers.

Each tab is an independent connection with its own port, baud rate, log, send
history and auto-(re)connect settings. Devices are remembered by USB identity, so
a tab follows its board when Windows assigns it a different COM port.

    python -m pip install pyside6 pyserial
    python tools/serial_monitor.py

Shortcuts: Ctrl+T new tab, Ctrl+W close tab, Ctrl+Tab / Ctrl+Shift+Tab switch tabs,
Ctrl+L clear log, Ctrl+S save log, Up/Down browse sent messages.
"""

import codecs
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
    from PySide6 import QtCore, QtGui, QtWidgets
except ImportError:
    raise SystemExit("Missing dependency. Run: python -m pip install pyside6 pyserial")

Qt = QtCore.Qt

SETTINGS = Path(__file__).with_name("serial_monitor_settings.json")
BAUDS = ("1200", "2400", "4800", "9600", "19200", "38400", "57600", "115200",
         "230400", "460800", "921600")
ENDINGS = {"None": "", "LF (\\n)": "\n", "CR (\\r)": "\r", "CRLF (\\r\\n)": "\r\n"}
# Console commands used most with this firmware (see docs/TOOLS.md).
COMMANDS = ("info", "list", "diag on", "diag off", "ota status")

COLORS = {
    "window": "#11161c", "surface": "#171e26", "raised": "#1f2833", "border": "#2b3644",
    "text": "#e4edf5", "muted": "#8796a8", "accent": "#4c9aff", "log": "#0d1218",
    "info": "#8cb9d9", "sent": "#8cdbad", "error": "#ff9d9d", "time": "#5f7183",
    "idle": "#5f6b7a", "busy": "#f0b45a", "on": "#5fd38d", "fail": "#ff7b7b",
}


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
        # serial_for_url also accepts pyserial URLs such as loop:// for testing.
        device = serial.serial_for_url(port, baudrate=baud, timeout=0.1, write_timeout=1)
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


def dot_icon(color, unread=False, cache={}):
    """Tab state dot; an accent ring marks a background tab with unseen data."""
    if (color, unread) not in cache:
        ratio = QtGui.QGuiApplication.primaryScreen().devicePixelRatio()
        pixmap = QtGui.QPixmap(int(12 * ratio), int(12 * ratio))
        pixmap.setDevicePixelRatio(ratio)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QtGui.QPainter(pixmap)
        painter.setRenderHint(QtGui.QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QtGui.QColor(color))
        painter.drawEllipse(QtCore.QRectF(3, 3, 6, 6) if unread else QtCore.QRectF(2, 2, 8, 8))
        if unread:
            painter.setPen(QtGui.QPen(QtGui.QColor(COLORS["accent"]), 1.5))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawEllipse(QtCore.QRectF(0.75, 0.75, 10.5, 10.5))
        painter.end()
        cache[color, unread] = QtGui.QIcon(pixmap)
    return cache[color, unread]


def apply_theme(app):
    app.setStyle("Fusion")
    c = COLORS
    palette = QtGui.QPalette()
    role = QtGui.QPalette.ColorRole
    for name, color in ((role.Window, c["window"]), (role.WindowText, c["text"]),
                        (role.Base, c["surface"]), (role.AlternateBase, c["raised"]),
                        (role.Text, c["text"]), (role.Button, c["raised"]),
                        (role.ButtonText, c["text"]), (role.ToolTipBase, c["raised"]),
                        (role.ToolTipText, c["text"]), (role.Highlight, c["accent"]),
                        (role.HighlightedText, "#ffffff"), (role.PlaceholderText, c["muted"]),
                        (role.Link, c["accent"])):
        palette.setColor(name, QtGui.QColor(color))
    for name in (role.Text, role.WindowText, role.ButtonText):
        palette.setColor(QtGui.QPalette.ColorGroup.Disabled, name, QtGui.QColor(c["idle"]))
    app.setPalette(palette)
    app.setStyleSheet(f"""
        QToolTip {{ color: {c['text']}; background: {c['raised']}; border: 1px solid {c['border']};
                   padding: 4px; }}
        QLineEdit, QComboBox {{ background: {c['surface']}; border: 1px solid {c['border']};
                   border-radius: 6px; padding: 5px 8px; }}
        QLineEdit:focus, QComboBox:focus {{ border-color: {c['accent']}; }}
        QComboBox QAbstractItemView {{ background: {c['raised']}; border: 1px solid {c['border']};
                   selection-background-color: {c['accent']}; outline: 0; }}
        QPushButton, QToolButton {{ background: {c['raised']}; border: 1px solid {c['border']};
                   border-radius: 6px; padding: 5px 12px; }}
        QPushButton:hover, QToolButton:hover {{ border-color: {c['accent']}; }}
        QPushButton:pressed, QToolButton:pressed {{ background: {c['border']}; }}
        QPushButton:disabled, QToolButton:disabled {{ background: {c['surface']}; color: {c['idle']}; }}
        QToolButton::menu-indicator {{ image: none; width: 0; }}
        QPushButton#primary {{ background: {c['accent']}; border-color: {c['accent']};
                   color: white; font-weight: 600; min-width: 90px; }}
        QPushButton#primary:hover {{ background: #63a8ff; }}
        QPushButton#primary[connected="true"] {{ background: #3a2328; border-color: {c['fail']};
                   color: {c['fail']}; }}
        QPushButton#primary:disabled {{ background: {c['surface']}; border-color: {c['border']};
                   color: {c['idle']}; }}
        QToolButton#newTab {{ border: none; background: transparent; font-size: 16px;
                   padding: 2px 10px; color: {c['muted']}; }}
        QToolButton#newTab:hover {{ color: {c['text']}; background: {c['raised']}; }}
        QToolButton#closeTab {{ border: none; background: transparent; color: {c['muted']};
                   padding: 0; margin-left: 4px; border-radius: 4px; min-width: 18px; min-height: 18px; }}
        QToolButton#closeTab:hover {{ color: {c['text']}; background: {c['border']}; }}
        QPlainTextEdit#log {{ background: {c['log']}; border: 1px solid {c['border']};
                   border-radius: 8px; padding: 6px; selection-background-color: #264f78; }}
        QTabWidget::pane {{ border: none; border-top: 1px solid {c['border']}; }}
        QTabBar::tab {{ background: transparent; color: {c['muted']}; padding: 8px 12px;
                   border: none; border-bottom: 2px solid transparent; }}
        QTabBar::tab:selected {{ color: {c['text']}; border-bottom-color: {c['accent']}; }}
        QTabBar::tab:hover:!selected {{ color: {c['text']}; background: {c['surface']}; }}
        QStatusBar {{ background: {c['surface']}; color: {c['muted']};
                   border-top: 1px solid {c['border']}; }}
        QStatusBar QLabel {{ padding: 2px 6px; }}
        QLabel#caption {{ color: {c['muted']}; }}
    """)


def monospace_font():
    font = QtGui.QFont()
    font.setFamilies(["Cascadia Mono", "Consolas", "Menlo", "DejaVu Sans Mono", "monospace"])
    font.setStyleHint(QtGui.QFont.StyleHint.Monospace)
    font.setPointSize(10)
    return font


class HistoryLineEdit(QtWidgets.QLineEdit):
    """Message entry where Up/Down walk through what was sent before."""

    def __init__(self):
        super().__init__()
        self.history = []
        self.index = 0
        self.draft = ""

    def set_history(self, history):
        self.history = history[-100:]
        self.index = len(self.history)

    def remember(self, text):
        if text and (not self.history or text != self.history[-1]):
            self.history.append(text)
            del self.history[:-100]
        self.index = len(self.history)
        self.draft = ""

    def keyPressEvent(self, event):
        step = {Qt.Key.Key_Up: -1, Qt.Key.Key_Down: 1}.get(event.key())
        if step is None or not self.history:
            super().keyPressEvent(event)
            return
        if self.index == len(self.history):
            self.draft = self.text()
        self.index = max(0, min(len(self.history), self.index + step))
        self.setText(self.draft if self.index == len(self.history) else self.history[self.index])


class ConnectionTab(QtWidgets.QWidget):
    """One serial connection: device selection, log and send bar."""

    changed = QtCore.Signal()   # title, state or status text changed
    activity = QtCore.Signal()  # new data arrived

    def __init__(self, monitor, config=None):
        super().__init__()
        self.monitor = monitor
        self.worker = None
        self.stop = threading.Event()
        self.events = queue.Queue()
        self.outgoing = queue.Queue(maxsize=100)
        self.connected = False
        self.active_port = None
        self.closing = False
        self.target = None
        self.was_connected = False
        self.manual_disconnect = False
        self.next_attempt = 0
        self.retry_delay = 3
        self.last_error = None
        self.port_info = []
        self.port_rows = None
        self.state = "idle"
        self.status = "Disconnected"
        self.details = "Looking for devices…"
        self.at_line_start = True
        self.pending_cr = False
        self.build()
        self.apply_config(config or {})
        self.auto_connect.toggled.connect(self.auto_changed)
        self.auto_reconnect.toggled.connect(self.auto_changed)

    # ----- layout -------------------------------------------------------------------------

    def build(self):
        layout = QtWidgets.QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 10)
        layout.setSpacing(8)

        toolbar = QtWidgets.QHBoxLayout()
        toolbar.setSpacing(6)
        layout.addLayout(toolbar)
        toolbar.addWidget(self.caption("Port"))
        self.port_box = QtWidgets.QComboBox()
        self.port_box.setEditable(True)
        self.port_box.setInsertPolicy(QtWidgets.QComboBox.InsertPolicy.NoInsert)
        self.port_box.setMinimumContentsLength(28)
        self.port_box.setSizeAdjustPolicy(
            QtWidgets.QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.port_box.lineEdit().setPlaceholderText("Select a device…")
        self.port_box.activated.connect(self.selection_changed)
        self.port_box.lineEdit().editingFinished.connect(self.selection_changed)
        self.port_box.currentTextChanged.connect(self.show_device)
        toolbar.addWidget(self.port_box, 1)
        self.refresh_button = QtWidgets.QToolButton()
        self.refresh_button.setIcon(self.style().standardIcon(
            QtWidgets.QStyle.StandardPixmap.SP_BrowserReload))
        self.refresh_button.setToolTip("Rescan serial ports")
        self.refresh_button.clicked.connect(lambda: self.monitor.refresh(force=True))
        toolbar.addWidget(self.refresh_button)
        toolbar.addSpacing(12)
        toolbar.addWidget(self.caption("Baud"))
        self.baud_box = QtWidgets.QComboBox()
        self.baud_box.setEditable(True)
        self.baud_box.addItems(BAUDS)
        self.baud_box.setInsertPolicy(QtWidgets.QComboBox.InsertPolicy.NoInsert)
        self.baud_box.setValidator(QtGui.QIntValidator(1, 100_000_000, self))
        self.baud_box.setMinimumContentsLength(8)
        toolbar.addWidget(self.baud_box)
        toolbar.addSpacing(6)
        self.connect_button = QtWidgets.QPushButton("Connect")
        self.connect_button.setObjectName("primary")
        self.connect_button.setCursor(Qt.CursorShape.PointingHandCursor)
        self.connect_button.clicked.connect(self.toggle)
        toolbar.addWidget(self.connect_button)

        self.log = QtWidgets.QPlainTextEdit()
        self.log.setObjectName("log")
        self.log.setReadOnly(True)
        self.log.setUndoRedoEnabled(False)
        self.log.setFont(monospace_font())
        # Keep long-running sessions from growing the displayed log indefinitely.
        self.log.setMaximumBlockCount(50_000)
        self.log.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse |
                                         Qt.TextInteractionFlag.TextSelectableByKeyboard)
        layout.addWidget(self.log, 1)
        self.formats = {}
        for tag, color in ((None, COLORS["text"]), ("info", COLORS["info"]),
                           ("sent", COLORS["sent"]), ("error", COLORS["error"]),
                           ("time", COLORS["time"])):
            fmt = QtGui.QTextCharFormat()
            fmt.setForeground(QtGui.QColor(color))
            self.formats[tag] = fmt

        options = QtWidgets.QHBoxLayout()
        options.setSpacing(14)
        layout.addLayout(options)
        self.autoscroll = QtWidgets.QCheckBox("Auto-scroll")
        self.timestamps = QtWidgets.QCheckBox("Timestamps")
        self.timestamps.setToolTip("Prefix each received line with the time it arrived")
        self.auto_connect = QtWidgets.QCheckBox("Auto-connect")
        self.auto_connect.setToolTip("Open the selected device as soon as it appears")
        self.auto_reconnect = QtWidgets.QCheckBox("Auto-reconnect")
        self.auto_reconnect.setToolTip("Reopen the device after it disconnects or resets")
        for box in (self.autoscroll, self.timestamps, self.auto_connect, self.auto_reconnect):
            options.addWidget(box)
        self.autoscroll.toggled.connect(lambda on: on and self.scroll_to_end())
        options.addStretch(1)
        save = QtWidgets.QPushButton("Save log…")
        save.clicked.connect(self.save)
        clear = QtWidgets.QPushButton("Clear")
        clear.clicked.connect(self.clear)
        options.addWidget(save)
        options.addWidget(clear)

        sendbar = QtWidgets.QHBoxLayout()
        sendbar.setSpacing(6)
        layout.addLayout(sendbar)
        self.entry = HistoryLineEdit()
        self.entry.setFont(monospace_font())
        self.entry.setPlaceholderText("Type a command and press Enter")
        self.entry.returnPressed.connect(self.send)
        sendbar.addWidget(self.entry, 1)
        self.commands = QtWidgets.QToolButton()
        self.commands.setText("Commands ▾")
        self.commands.setPopupMode(QtWidgets.QToolButton.ToolButtonPopupMode.InstantPopup)
        menu = QtWidgets.QMenu(self.commands)
        for command in COMMANDS:
            menu.addAction(command, lambda command=command: self.send(command))
        self.commands.setMenu(menu)
        sendbar.addWidget(self.commands)
        self.ending_box = QtWidgets.QComboBox()
        self.ending_box.addItems(list(ENDINGS))
        self.ending_box.setToolTip("Line ending appended to each message")
        sendbar.addWidget(self.ending_box)
        self.send_button = QtWidgets.QPushButton("Send")
        self.send_button.clicked.connect(self.send)
        sendbar.addWidget(self.send_button)
        self.set_connected_ui(False)

    @staticmethod
    def caption(text):
        label = QtWidgets.QLabel(text)
        label.setObjectName("caption")
        return label

    def apply_config(self, config):
        if isinstance(config.get("target"), dict):
            self.target = config["target"]
        self.port_box.setEditText(str(config.get("port", "")))
        self.baud_box.setCurrentText(str(config.get("baud", "115200")))
        self.auto_connect.setChecked(config.get("auto_connect") is True)
        self.auto_reconnect.setChecked(config.get("auto_reconnect", True) is True)
        self.autoscroll.setChecked(config.get("autoscroll", True) is True)
        self.timestamps.setChecked(config.get("timestamps") is True)
        self.ending_box.setCurrentText(config.get("ending") if config.get("ending") in ENDINGS
                                       else "LF (\\n)")
        if isinstance(config.get("history"), list):
            self.entry.set_history([str(item) for item in config["history"]])

    def config(self):
        return {"port": self.selected_port(), "baud": self.baud_box.currentText(),
                "target": self.target, "auto_connect": self.auto_connect.isChecked(),
                "auto_reconnect": self.auto_reconnect.isChecked(),
                "autoscroll": self.autoscroll.isChecked(),
                "timestamps": self.timestamps.isChecked(),
                "ending": self.ending_box.currentText(), "history": self.entry.history}

    def set_connected_ui(self, connected):
        """Lock the device settings while a worker owns the port."""
        busy = self.worker is not None
        for widget in (self.port_box, self.baud_box, self.refresh_button):
            widget.setEnabled(not busy)
        for widget in (self.send_button, self.commands):
            widget.setEnabled(connected)
        self.connect_button.setText("Disconnect" if busy else "Connect")
        self.connect_button.setProperty("connected", busy)
        self.connect_button.style().unpolish(self.connect_button)
        self.connect_button.style().polish(self.connect_button)

    def set_status(self, text, state=None):
        self.status = text
        if state:
            self.state = state
        self.changed.emit()

    def title(self):
        device = self.active_port or self.selected_port()
        if not device:
            return "New connection"
        port = next((p for p in self.port_info if p.device == device), None)
        board = getattr(port, "board_name", "")
        if board and board != "Unknown device":
            board = board if len(board) <= 22 else board[:21] + "…"
            return f"{device} · {board}"
        return device

    # ----- device selection ---------------------------------------------------------------

    def update_ports(self, ports):
        selected = self.selected_port()
        self.port_info = ports
        target_port = find_target(ports, self.target)
        if self.worker is None and target_port:
            selected = target_port.device
        rows = [(p.device, p.board_name) for p in ports]
        if rows != self.port_rows or selected != self.selected_port():
            self.port_rows = rows
            box = self.port_box
            box.blockSignals(True)
            box.clear()
            for port in ports:
                box.addItem(f"{port.device} — {port.board_name}", port.device)
                box.setItemData(box.count() - 1, self.describe(port), Qt.ItemDataRole.ToolTipRole)
            index = box.findData(selected)
            box.setCurrentIndex(index)
            if index < 0:
                # Leave selection empty on first launch: do not open an arbitrary device.
                box.setEditText(selected)
            box.blockSignals(False)
        self.show_device()

    @staticmethod
    def describe(port):
        details = [f"Board: {port.board_name}"]
        if port.manufacturer:
            details.append(f"Manufacturer: {port.manufacturer}")
        if port.serial_number:
            details.append(f"Serial: {port.serial_number}")
        if port.vid is not None and port.pid is not None:
            details.append(f"USB: {port.vid:04X}:{port.pid:04X}")
        return "  •  ".join(details)

    def show_device(self, *_):
        port = next((p for p in self.port_info if p.device == self.selected_port()), None)
        if port is not None:
            self.details = self.describe(port)
        elif self.selected_port():
            self.details = "Selected device is unavailable."
        else:
            self.details = "Select a device to connect."
        self.changed.emit()

    def select_free_port(self, used):
        if self.selected_port():
            return
        port = next((p for p in self.port_info if p.device not in used), None)
        if port is not None:
            self.port_box.setCurrentIndex(self.port_box.findData(port.device))

    def selected_port(self):
        text = self.port_box.currentText().strip()
        index = self.port_box.findText(text)
        return self.port_box.itemData(index) if index >= 0 else text

    def selection_changed(self, *_):
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
        enabled = (self.auto_reconnect if self.was_connected else self.auto_connect).isChecked()
        if not enabled or not self.selected_port():
            return
        if not self.target:
            self.selection_changed()
        if find_target(self.port_info, self.target) is None:
            if self.status != "Waiting for selected device…":
                self.set_status("Waiting for selected device…", "busy")
        elif time.monotonic() >= self.next_attempt:
            self.open_connection(automatic=True)

    # ----- connection ---------------------------------------------------------------------

    def toggle(self):
        if self.worker is not None:
            self.manual_disconnect = True
            self.stop.set()
            self.connected = False
            self.set_connected_ui(False)
            self.connect_button.setEnabled(False)
            self.set_status("Disconnecting…", "busy")
            return
        self.manual_disconnect = False
        self.open_connection()

    def open_connection(self, automatic=False):
        port = self.selected_port()
        try:
            baud = int(self.baud_box.currentText())
            if not port or baud <= 0:
                raise ValueError
        except ValueError:
            self.fail("Enter a port and a positive whole-number baud rate.", automatic)
            return
        owner = self.monitor.port_owner(port, self)
        if owner is not None:
            self.fail(f"{port} is already open in the “{owner.title()}” tab.", automatic)
            return
        info = next((p for p in self.port_info if p.device == port), None)
        self.target = identity(info, self.port_info) if info else {"device": port}
        self.stop = threading.Event()
        self.outgoing = queue.Queue(maxsize=100)
        self.active_port = port
        self.last_error = None
        self.worker = threading.Thread(target=serial_worker,
                                       args=(port, baud, self.outgoing, self.events, self.stop),
                                       daemon=True)
        self.worker.start()
        self.set_connected_ui(False)
        self.set_status(f"Opening {port}…", "busy")

    def fail(self, message, automatic):
        self.set_status(message, "fail")
        if not automatic:
            QtWidgets.QMessageBox.warning(self, "Cannot connect", message)
        self.next_attempt = time.monotonic() + 3

    def send(self, message=None):
        if not self.connected:
            return
        typed = not isinstance(message, str)
        if typed:
            message = self.entry.text()
        payload = (message + ENDINGS[self.ending_box.currentText()]).encode("utf-8")
        if not payload:
            return
        try:
            self.outgoing.put_nowait(payload)
        except queue.Full:
            QtWidgets.QMessageBox.warning(self, "Send queue full",
                                          "Wait for pending messages to finish sending.")
            return
        self.entry.remember(message)
        if typed:
            self.entry.clear()
        self.entry.setFocus()

    def shutdown(self, wait=0.0):
        self.closing = True
        self.stop.set()
        if self.worker is not None and wait:
            self.worker.join(wait)

    def poll(self):
        received = []
        for _ in range(500):
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "received":
                received.append(value)
                continue
            if received:
                self.append_received("".join(received))
                received = []
            if kind == "connected":
                if not self.stop.is_set():
                    self.connected = True
                    self.was_connected = True
                    self.retry_delay = 3
                    self.set_connected_ui(True)
                    self.set_status(value + " • 8N1 • UTF-8", "on")
                self.note(value)
            elif kind == "sent":
                self.note("TX " + repr(value.decode("utf-8")), "sent")
            elif kind == "error":
                self.last_error = value
                self.note(value, "error")
            elif kind == "closed":
                self.worker = None
                self.active_port = None
                self.connected = False
                self.set_connected_ui(False)
                self.connect_button.setEnabled(True)
                if self.last_error:
                    self.set_status(f"Disconnected: {self.last_error}", "fail")
                else:
                    self.set_status("Disconnected", "idle")
                self.note("Disconnected")
                self.next_attempt = time.monotonic() + self.retry_delay
                self.retry_delay = min(self.retry_delay * 2, 30)
        if received:
            self.append_received("".join(received))

    # ----- log ----------------------------------------------------------------------------

    def insert(self, text, tag=None):
        cursor = QtGui.QTextCursor(self.log.document())
        cursor.movePosition(QtGui.QTextCursor.MoveOperation.End)
        cursor.insertText(text, self.formats[tag])

    def scroll_to_end(self):
        bar = self.log.verticalScrollBar()
        bar.setValue(bar.maximum())

    def append_received(self, text):
        # Show CR, LF and CRLF line endings alike, also when CRLF is split across reads.
        if self.pending_cr and text.startswith("\n"):
            text = text[1:]
        self.pending_cr = text.endswith("\r")
        text = text.replace("\r\n", "\n").replace("\r", "\n")
        if not text:
            return
        if self.timestamps.isChecked():
            for piece in re.findall(r"[^\n]*\n|[^\n]+", text):
                if self.at_line_start:
                    self.insert(f"{datetime.now():%H:%M:%S.%f}"[:12] + "  ", "time")
                self.insert(piece)
                self.at_line_start = piece.endswith("\n")
        else:
            self.insert(text)
            self.at_line_start = text.endswith("\n")
        if self.autoscroll.isChecked():
            self.scroll_to_end()
        self.activity.emit()

    def note(self, text, tag="info"):
        prefix = "" if self.at_line_start else "\n"
        self.insert(f"{prefix}[{datetime.now():%H:%M:%S}] {text}\n", tag)
        self.at_line_start = True
        if self.autoscroll.isChecked():
            self.scroll_to_end()

    def clear(self):
        self.log.clear()
        self.at_line_start = True

    def save(self):
        name = f"{self.selected_port() or 'serial'}-{datetime.now():%Y%m%d-%H%M%S}.txt"
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Save log", name, "Text log (*.txt);;All files (*.*)")
        if path:
            try:
                Path(path).write_text(self.log.toPlainText(), encoding="utf-8")
            except OSError as error:
                QtWidgets.QMessageBox.critical(self, "Save failed", str(error))


class SerialMonitor(QtWidgets.QMainWindow):
    """Window with one ConnectionTab per serial connection and a shared port scanner."""

    def __init__(self):
        super().__init__()
        self.scan_events = queue.Queue()
        self.scanning = False
        self.next_scan = 0
        self.name_cache = {}
        self.ports = None
        self.setWindowTitle("Serial Monitor")
        self.resize(980, 640)
        self.setMinimumSize(760, 420)

        self.tabs = QtWidgets.QTabWidget()
        self.tabs.setMovable(True)
        self.tabs.setDocumentMode(True)
        self.tabs.setElideMode(Qt.TextElideMode.ElideRight)
        self.tabs.tabBar().setExpanding(False)
        self.tabs.tabBar().setDrawBase(False)
        self.tabs.currentChanged.connect(self.current_changed)
        plus = QtWidgets.QToolButton()
        plus.setObjectName("newTab")
        plus.setText("+")
        plus.setToolTip("New connection (Ctrl+T)")
        plus.clicked.connect(lambda: self.new_tab(select_free=True))
        self.tabs.setCornerWidget(plus, Qt.Corner.TopRightCorner)
        self.setCentralWidget(self.tabs)

        self.status_label = QtWidgets.QLabel()
        self.details_label = QtWidgets.QLabel()
        self.details_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        self.statusBar().setSizeGripEnabled(False)
        self.statusBar().addWidget(self.status_label, 1)
        self.statusBar().addPermanentWidget(self.details_label)

        for keys, action in (("Ctrl+T", lambda: self.new_tab(select_free=True)),
                             ("Ctrl+W", lambda: self.close_tab(self.tabs.currentIndex())),
                             ("Ctrl+Tab", lambda: self.cycle(1)),
                             ("Ctrl+Shift+Tab", lambda: self.cycle(-1)),
                             ("Ctrl+L", lambda: self.current().clear()),
                             ("Ctrl+S", lambda: self.current().save())):
            QtGui.QShortcut(QtGui.QKeySequence(keys), self, action)

        settings = self.load_settings()
        for config in settings.get("tabs", []):
            if isinstance(config, dict):
                self.new_tab(config)
        if not self.tabs.count():
            self.new_tab()
        current = settings.get("current")
        if isinstance(current, int) and 0 <= current < self.tabs.count():
            self.tabs.setCurrentIndex(current)
        if isinstance(settings.get("geometry"), str):
            self.restoreGeometry(QtCore.QByteArray.fromBase64(settings["geometry"].encode()))

        self.refresh()
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(50)

    @staticmethod
    def load_settings():
        try:
            settings = json.loads(SETTINGS.read_text(encoding="utf-8"))
            if not isinstance(settings, dict):
                raise ValueError("Invalid settings")
        except (OSError, ValueError, TypeError):
            return {}
        if "tabs" not in settings and "port" in settings:
            settings = {"tabs": [settings]}  # single-connection settings from the tkinter version
        return settings

    def save_settings(self):
        settings = {"tabs": [tab.config() for tab in self.all_tabs()],
                    "current": self.tabs.currentIndex(),
                    "geometry": bytes(self.saveGeometry().toBase64()).decode()}
        try:
            SETTINGS.write_text(json.dumps(settings, indent=2), encoding="utf-8")
        except OSError as error:
            QtWidgets.QMessageBox.warning(self, "Settings not saved", str(error))

    # ----- tabs ---------------------------------------------------------------------------

    def all_tabs(self):
        return [self.tabs.widget(index) for index in range(self.tabs.count())]

    def current(self):
        return self.tabs.currentWidget()

    def new_tab(self, config=None, select_free=False):
        tab = ConnectionTab(self, config)
        tab.changed.connect(lambda: self.tab_changed(tab))
        tab.activity.connect(lambda: self.tab_activity(tab))
        tab.unread = False
        index = self.tabs.addTab(tab, "")
        close = QtWidgets.QToolButton()
        close.setObjectName("closeTab")
        close.setText("✕")
        close.setToolTip("Close connection (Ctrl+W)")
        close.setCursor(Qt.CursorShape.PointingHandCursor)
        close.clicked.connect(lambda: self.close_tab(self.tabs.indexOf(tab)))
        self.tabs.tabBar().setTabButton(index, QtWidgets.QTabBar.ButtonPosition.RightSide, close)
        self.tabs.setCurrentIndex(index)
        if self.ports is not None:
            tab.update_ports(self.ports)
            if select_free:
                tab.select_free_port({other.selected_port() for other in self.all_tabs()
                                      if other is not tab})
        self.tab_changed(tab)
        tab.port_box.setFocus()
        return tab

    def close_tab(self, index):
        tab = self.tabs.widget(index)
        if tab is None:
            return
        tab.shutdown(wait=1.0)
        self.tabs.removeTab(index)
        tab.deleteLater()
        if not self.tabs.count():
            self.new_tab(select_free=True)

    def cycle(self, step):
        self.tabs.setCurrentIndex((self.tabs.currentIndex() + step) % self.tabs.count())

    def tab_changed(self, tab):
        index = self.tabs.indexOf(tab)
        if index < 0:
            return
        self.tabs.setTabIcon(index, dot_icon(COLORS[tab.state], tab.unread))
        self.tabs.setTabText(index, tab.title())
        self.tabs.setTabToolTip(index, f"{tab.status}\n{tab.details}")
        if tab is self.current():
            self.status_label.setText(tab.status)
            self.details_label.setText(tab.details)
            self.setWindowTitle(f"{tab.title()} — Serial Monitor")

    def tab_activity(self, tab):
        if tab is not self.current() and not tab.unread:
            tab.unread = True
            self.tab_changed(tab)

    def current_changed(self, index):
        tab = self.tabs.widget(index)
        if tab is not None:
            tab.unread = False
            self.tab_changed(tab)

    def port_owner(self, device, exclude):
        return next((tab for tab in self.all_tabs()
                     if tab is not exclude and tab.worker is not None and tab.active_port == device),
                    None)

    # ----- scanning and polling -----------------------------------------------------------

    def refresh(self, force=False):
        if force:
            self.next_scan = 0
        if not self.scanning and time.monotonic() >= self.next_scan:
            self.scanning = True
            self.next_scan = time.monotonic() + 3
            threading.Thread(target=discover, args=(self.scan_events, self.name_cache),
                             daemon=True).start()

    def poll(self):
        while True:
            try:
                kind, value = self.scan_events.get_nowait()
            except queue.Empty:
                break
            self.scanning = False
            if kind == "ports":
                self.ports = value
                for tab in self.all_tabs():
                    tab.update_ports(value)
            elif kind == "scan_error":
                for tab in self.all_tabs():
                    if tab.worker is None:
                        tab.set_status(f"Device discovery failed: {value}", "fail")
        for tab in self.all_tabs():
            tab.poll()
            tab.automatic_connect()
        self.refresh()

    def closeEvent(self, event):
        self.save_settings()
        self.timer.stop()
        tabs = self.all_tabs()
        for tab in tabs:
            tab.shutdown()
        # Give workers a moment to close their ports cleanly; they are daemons otherwise.
        deadline = time.monotonic() + 2
        for tab in tabs:
            if tab.worker is not None:
                tab.worker.join(max(0.0, deadline - time.monotonic()))
        event.accept()


def main():
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("Serial Monitor")
    apply_theme(app)
    window = SerialMonitor()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
