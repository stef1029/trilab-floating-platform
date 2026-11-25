"""Serial console: read a board's human debug text from a COM port.

The hub prints its debug text over native USB (CDC ACM).  It appears as a
COM port that vanishes and returns whenever the hub resets, so the console
can reopen the same port automatically.
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from PySide6.QtCore import QIODevice, QTimer
from PySide6.QtGui import QFont, QTextCursor
from PySide6.QtSerialPort import QSerialPort, QSerialPortInfo
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
)

# USB vendor IDs, used to label ports and pick a sensible default.
ZEPHYR_VID = 0x2FE3   # Zephyr USB device stack: the hub's own USB port
SEGGER_VID = 0x1366   # J-Link virtual COM port (DK builds print here)
ST_VID = 0x0483       # ST-Link virtual COM port
_VENDOR_NAMES = {ZEPHYR_VID: "hub USB", SEGGER_VID: "J-Link", ST_VID: "ST-Link"}

BAUD = 115200          # ignored by USB CDC; used by J-Link / ST-Link ports
MAX_LINES = 20_000
RECONNECT_MS = 1000


def _port_label(info: QSerialPortInfo) -> str:
    vendor = info.vendorIdentifier() if info.hasVendorIdentifier() else None
    name = _VENDOR_NAMES.get(vendor, info.description() or "serial port")
    return f"{info.portName()} — {name}"


class SerialConsole(QFrame):
    def __init__(self) -> None:
        super().__init__()
        self.port = QSerialPort(self)
        self.port.readyRead.connect(self._read)
        self.port.errorOccurred.connect(self._error)
        self._buffer = bytearray()
        self._wanted: str | None = None  # port the user asked to keep open

        self.ports = QComboBox()
        self.ports.setMinimumWidth(260)
        refresh = QPushButton("Refresh")
        refresh.setObjectName("secondaryButton")
        refresh.clicked.connect(self.refresh_ports)
        self.connect_button = QPushButton("Open")
        self.connect_button.clicked.connect(self._toggle)
        self.reconnect = QCheckBox("Reopen after reset")
        self.reconnect.setChecked(True)
        self.reconnect.setToolTip(
            "The hub's USB port disappears when it resets; reopen it when it returns")
        self.timestamps = QCheckBox("PC timestamps")
        self.autoscroll = QCheckBox("Follow")
        self.autoscroll.setChecked(True)
        clear = QPushButton("Clear")
        clear.setObjectName("secondaryButton")
        save = QPushButton("Save…")
        save.setObjectName("secondaryButton")
        save.clicked.connect(self._save)
        self.status = QLabel("Closed")
        self.status.setObjectName("muted")

        self.text = QPlainTextEdit()
        self.text.setReadOnly(True)
        self.text.setMaximumBlockCount(MAX_LINES)
        self.text.setLineWrapMode(QPlainTextEdit.NoWrap)
        font = QFont("Consolas")
        font.setStyleHint(QFont.Monospace)
        self.text.setFont(font)
        clear.clicked.connect(self.text.clear)

        controls = QHBoxLayout()
        controls.addWidget(QLabel("Port"))
        controls.addWidget(self.ports)
        controls.addWidget(refresh)
        controls.addWidget(self.connect_button)
        controls.addWidget(self.reconnect)
        controls.addStretch()
        controls.addWidget(self.timestamps)
        controls.addWidget(self.autoscroll)
        controls.addWidget(clear)
        controls.addWidget(save)

        layout = QVBoxLayout(self)
        layout.addLayout(controls)
        layout.addWidget(self.status)
        layout.addWidget(self.text, 1)

        self.reconnect_timer = QTimer(self)
        self.reconnect_timer.timeout.connect(self._try_reopen)
        self.reconnect_timer.start(RECONNECT_MS)
        self.refresh_ports()

    # -- ports ---------------------------------------------------------------

    def refresh_ports(self) -> None:
        current = self.ports.currentData()
        self.ports.clear()
        default = -1
        for info in QSerialPortInfo.availablePorts():
            self.ports.addItem(_port_label(info), info.portName())
            if info.hasVendorIdentifier() and info.vendorIdentifier() == ZEPHYR_VID:
                default = self.ports.count() - 1
        index = self.ports.findData(current) if current else -1
        self.ports.setCurrentIndex(index if index >= 0 else max(default, 0))

    def _toggle(self) -> None:
        if self.port.isOpen() or self._wanted:
            self.close_port()
        else:
            name = self.ports.currentData()
            if name:
                self._wanted = name
                self._open(name)

    def _open(self, name: str) -> bool:
        self.port.setPortName(name)
        self.port.setBaudRate(BAUD)
        if not self.port.open(QIODevice.ReadOnly):
            self._set_status(f"Could not open {name}: {self.port.errorString()}")
            self._update_button()
            return False
        # Zephyr CDC ACM sends nothing until the host raises DTR.
        self.port.setDataTerminalReady(True)
        self._buffer.clear()
        self._set_status(f"Open: {name}")
        self._update_button()
        return True

    def close_port(self) -> None:
        self._wanted = None
        if self.port.isOpen():
            self.port.close()
        self._flush_partial()
        self._set_status("Closed")
        self._update_button()

    def _try_reopen(self) -> None:
        if self.port.isOpen() or not self._wanted or not self.reconnect.isChecked():
            return
        names = {info.portName() for info in QSerialPortInfo.availablePorts()}
        if self._wanted in names and self._open(self._wanted):
            self._append_marker(f"reopened {self._wanted}")

    def _error(self, error: QSerialPort.SerialPortError) -> None:
        if error in (QSerialPort.NoError, QSerialPort.TimeoutError):
            return
        if error == QSerialPort.ResourceError and self.port.isOpen():
            # The device went away (unplugged or reset).
            self.port.close()
            self._flush_partial()
            self._append_marker(f"{self._wanted} disconnected")
            if self._wanted and self.reconnect.isChecked():
                self._set_status(f"Waiting for {self._wanted} to come back…")
            else:
                self._wanted = None
                self._set_status("Disconnected")
            self._update_button()

    # -- text ----------------------------------------------------------------

    def _read(self) -> None:
        self._buffer += bytes(self.port.readAll().data())
        *lines, rest = self._buffer.split(b"\n")
        self._buffer = bytearray(rest)
        for line in lines:
            self._append(line.decode("utf-8", errors="replace").rstrip("\r"))

    def _flush_partial(self) -> None:
        if self._buffer:
            self._append(self._buffer.decode("utf-8", errors="replace").rstrip("\r"))
            self._buffer.clear()

    def _append(self, line: str) -> None:
        if self.timestamps.isChecked():
            line = f"{datetime.now():%H:%M:%S.%f}"[:-3] + "  " + line
        scrollbar = self.text.verticalScrollBar()
        position = scrollbar.value()
        self.text.appendPlainText(line)
        if self.autoscroll.isChecked():
            self.text.moveCursor(QTextCursor.End)
            scrollbar.setValue(scrollbar.maximum())
        else:
            scrollbar.setValue(position)

    def _append_marker(self, text: str) -> None:
        self._append(f"──── {text} ────")

    def _save(self) -> None:
        path, _ = QFileDialog.getSaveFileName(
            self, "Save serial log",
            f"serial_{datetime.now():%Y%m%d_%H%M%S}.txt", "Text (*.txt)")
        if path:
            Path(path).write_text(self.text.toPlainText(), encoding="utf-8")
            self._set_status(f"Saved to {path}")

    def _set_status(self, text: str) -> None:
        self.status.setText(text)

    def _update_button(self) -> None:
        active = self.port.isOpen() or bool(self._wanted)
        self.connect_button.setText("Close" if active else "Open")
        self.ports.setEnabled(not active)
