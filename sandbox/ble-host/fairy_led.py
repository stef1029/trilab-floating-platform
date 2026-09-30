"""Drive a Fairy's LED through the bridge, using the real Transport v1.

    PC --BLE--> bridge (nRF52840) --UART 460800--> fairy-lite (Nucleo) --> LED

Frames are built by adelie.transport -- the same module Adelie uses -- so the
bytes on air and on the wire are genuinely Transport v1: 12-byte header,
CRC16-CCITT, real addresses.

Run:
    cd sandbox/ble-host
    run_fairy            (Windows cmd)
    ./run_fairy.sh       (git bash)
"""

from __future__ import annotations

import asyncio
import sys
import threading

from bleak import BleakClient, BleakScanner
from PySide6 import QtCore, QtWidgets

from adelie.transport import Frame, decode_frame, encode_frame

DEVICE_NAME = "sandbox-bridge"
RX_UUID = "5b1e0012-f00d-4a5e-9c33-0a1b2c3d4e5f"   # PC -> bridge
TX_UUID = "5b1e0013-f00d-4a5e-9c33-0a1b2c3d4e5f"   # bridge -> PC

# Real addresses, from firmware/shared/include/fairy_shared/system_config.hpp
ADELIE_ADDRESS = 0x03
FAIRY0_ADDRESS = 0x10

CHANNEL_ADELIE = 2          # fairy::transport::Channel::adelie
FLAG_FIRST = 1 << 4
FLAG_LAST = 1 << 5

OP_PING = 0x01
OP_SET_LED = 0x02


class BleWorker(QtCore.QObject):
    state_changed = QtCore.Signal(str)
    reply_received = QtCore.Signal(str)

    def __init__(self) -> None:
        super().__init__()
        self._loop = asyncio.new_event_loop()
        self._client: BleakClient | None = None
        self._transfer = 0
        threading.Thread(target=self._run_loop, daemon=True).start()

    def _run_loop(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_forever()

    # ---------------------------------------------------------- public API
    def connect(self) -> None:
        self._submit(self._connect())

    def set_led(self, on: bool) -> None:
        self._submit(self._send(bytes([OP_SET_LED, 1 if on else 0])))

    def ping(self) -> None:
        self._submit(self._send(bytes([OP_PING])))

    def _submit(self, coro) -> None:
        future = asyncio.run_coroutine_threadsafe(coro, self._loop)
        future.add_done_callback(
            lambda f: (f.exception() is not None)
            and self.state_changed.emit(
                f"{type(f.exception()).__name__}: {f.exception()}"))

    # ------------------------------------------------------------ internals
    def _next_transfer(self) -> int:
        self._transfer = (self._transfer + 1) & 0xFFFF or 1
        return self._transfer

    async def _connect(self) -> None:
        self.state_changed.emit(f"scanning for {DEVICE_NAME}...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=20)
        if device is None:
            self.state_changed.emit(f"not found: is {DEVICE_NAME} powered?")
            return

        client = BleakClient(device, timeout=20,
                             disconnected_callback=self._on_disconnected)
        await client.connect()
        self._client = client
        await client.start_notify(TX_UUID, self._on_notify)
        self.state_changed.emit(f"connected (MTU {client.mtu_size})")

    async def _send(self, payload: bytes) -> None:
        if self._client is None or not self._client.is_connected:
            self.state_changed.emit("not connected")
            return

        frame = Frame(
            channel=CHANNEL_ADELIE,
            flags=FLAG_FIRST | FLAG_LAST,
            source=ADELIE_ADDRESS,
            destination=FAIRY0_ADDRESS,
            fragment_index=0,
            fragment_count=1,
            transfer_id=self._next_transfer(),
            payload=payload,
        )
        raw = encode_frame(frame)
        self.state_changed.emit(
            f"-> dst=0x{FAIRY0_ADDRESS:02x} transfer={frame.transfer_id} "
            f"payload={payload.hex()} ({len(raw)} bytes on air)")
        await self._client.write_gatt_char(RX_UUID, raw, response=True)

    def _on_notify(self, _characteristic, data: bytearray) -> None:
        try:
            frame = decode_frame(bytes(data))
        except Exception as error:                      # noqa: BLE001
            self.reply_received.emit(f"undecodable reply: {error}")
            return
        self.reply_received.emit(
            f"<- src=0x{frame.source:02x} transfer={frame.transfer_id} "
            f"payload={bytes(frame.payload).hex()}")

    def _on_disconnected(self, _client: BleakClient) -> None:
        self._client = None
        self.state_changed.emit("disconnected")


class Window(QtWidgets.QWidget):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("sandbox: Fairy LED over Transport v1")

        self.status = QtWidgets.QLabel("idle")
        self.status.setWordWrap(True)
        self.reply = QtWidgets.QLabel("no replies yet")
        self.reply.setWordWrap(True)

        connect_button = QtWidgets.QPushButton("Connect")
        ping_button = QtWidgets.QPushButton("Ping Fairy 0")
        self.led_button = QtWidgets.QPushButton("Fairy LED off")
        self.led_button.setCheckable(True)

        layout = QtWidgets.QVBoxLayout(self)
        for widget in (self.status, connect_button, ping_button,
                       self.led_button, self.reply):
            layout.addWidget(widget)

        self.worker = BleWorker()
        self.worker.state_changed.connect(self.status.setText)
        self.worker.reply_received.connect(self.reply.setText)

        connect_button.clicked.connect(self.worker.connect)
        ping_button.clicked.connect(self.worker.ping)
        self.led_button.toggled.connect(self._on_led_toggled)

    def _on_led_toggled(self, checked: bool) -> None:
        self.led_button.setText("Fairy LED on" if checked else "Fairy LED off")
        self.worker.set_led(checked)


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    window = Window()
    window.resize(460, 220)
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
