"""Minimal host for the sandbox BLE peripheral -- a stripped-down Adelie.

    button in the GUI  ->  BLE write   ->  LED1 on the DK
    BUTTON1 on the DK  ->  notification ->  counter in the GUI

Run with the sandbox venv:

    cd sandbox/ble-host
    ..\\..\\software\\.venv\\Scripts\\python.exe led_button.py

Everything Adelie does on top of this -- Transport v1 framing, CRC16,
fragmentation, the record protocols -- is deliberately absent so the BLE
plumbing is visible on its own.
"""

from __future__ import annotations

import asyncio
import sys
import threading

from bleak import BleakClient, BleakScanner
from PySide6 import QtCore, QtWidgets

DEVICE_NAME = "sandbox-led"
LED_UUID = "5b1e0002-f00d-4a5e-9c33-0a1b2c3d4e5f"
BUTTON_UUID = "5b1e0003-f00d-4a5e-9c33-0a1b2c3d4e5f"


class BleWorker(QtCore.QObject):
    """Owns the asyncio loop and the BLE connection on its own thread.

    Qt must stay on the main thread and asyncio wants its own, so the two
    talk through Qt signals (thread-safe) and run_coroutine_threadsafe.
    """

    state_changed = QtCore.Signal(str)
    button_notified = QtCore.Signal(int)

    def __init__(self) -> None:
        super().__init__()
        self._loop = asyncio.new_event_loop()
        self._client: BleakClient | None = None
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()

    def _run_loop(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    # ---------------------------------------------------------- public API
    def connect(self) -> None:
        asyncio.run_coroutine_threadsafe(self._connect(), self._loop)

    def set_led(self, on: bool) -> None:
        asyncio.run_coroutine_threadsafe(self._set_led(on), self._loop)

    # ------------------------------------------------------------ coroutines
    async def _connect(self) -> None:
        self.state_changed.emit(f"scanning for {DEVICE_NAME}...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=20)
        if device is None:
            self.state_changed.emit(f"not found: is {DEVICE_NAME} powered?")
            return

        self.state_changed.emit(f"connecting to {device.address}...")
        client = BleakClient(device, timeout=20,
                             disconnected_callback=self._on_disconnected)
        await client.connect()
        self._client = client

        def on_notify(_characteristic, data: bytearray) -> None:
            self.button_notified.emit(int(data[0]))

        await client.start_notify(BUTTON_UUID, on_notify)
        self.state_changed.emit(f"connected (MTU {client.mtu_size})")

    async def _set_led(self, on: bool) -> None:
        if self._client is None or not self._client.is_connected:
            self.state_changed.emit("not connected")
            return
        # response=True asks the peripheral to acknowledge the write.
        await self._client.write_gatt_char(LED_UUID, bytes([1 if on else 0]),
                                           response=True)

    def _on_disconnected(self, _client: BleakClient) -> None:
        self._client = None
        self.state_changed.emit("disconnected")


class Window(QtWidgets.QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("sandbox BLE: LED + button")

        self.status = QtWidgets.QLabel("idle")
        self.presses = QtWidgets.QLabel("button presses: 0")
        connect_button = QtWidgets.QPushButton("Connect")
        self.led_button = QtWidgets.QPushButton("LED off")
        self.led_button.setCheckable(True)

        layout = QtWidgets.QVBoxLayout(self)
        for widget in (self.status, connect_button, self.led_button, self.presses):
            layout.addWidget(widget)

        self.worker = BleWorker()
        self.worker.state_changed.connect(self.status.setText)
        self.worker.button_notified.connect(
            lambda n: self.presses.setText(f"button presses: {n}"))

        connect_button.clicked.connect(self.worker.connect)
        self.led_button.toggled.connect(self._on_led_toggled)

    def _on_led_toggled(self, checked: bool) -> None:
        self.led_button.setText("LED on" if checked else "LED off")
        self.worker.set_led(checked)


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    window = Window()
    window.resize(320, 160)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
