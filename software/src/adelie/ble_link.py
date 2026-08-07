from __future__ import annotations

import asyncio
from collections.abc import Callable
from dataclasses import dataclass
import queue
import threading
import time
import traceback

from bleak import BleakClient, BleakScanner

from .constants import (
    ADELIE_ADDRESS,
    Channel,
    KORORA_NAME,
    RX_UUID,
    TX_UUID,
    TransportFlag,
)
from .transport import Message, Reassembler, decode_frame, fragment_message

MessageCallback = Callable[[Message, int], None]
StateCallback = Callable[[str, str], None]
SentCallback = Callable[[int, int], None]
WriteCompleteCallback = Callable[[int, int], None]


@dataclass(frozen=True, slots=True)
class BleDiagnostics:
    connected: bool = False
    att_write_size: int = 0
    fragment_payload: int = 0
    rx_notifications: int = 0
    rx_frames: int = 0
    rx_messages: int = 0
    rx_bytes: int = 0
    rx_decode_errors: int = 0
    rx_reassembly_errors: int = 0
    rx_address_errors: int = 0
    tx_messages: int = 0
    tx_fragments: int = 0
    tx_bytes: int = 0
    tx_write_errors: int = 0
    outgoing_queue: int = 0
    connected_since_ns: int = 0
    last_rx_ns: int = 0
    last_tx_ns: int = 0
    last_error: str = ""


class BleLink:
    def __init__(
        self,
        *,
        on_message: MessageCallback,
        on_state: StateCallback,
        on_sent: SentCallback | None = None,
        on_write_complete: WriteCompleteCallback | None = None,
        device_name: str = KORORA_NAME,
    ) -> None:
        self._on_message = on_message
        self._on_state = on_state
        self._on_sent = on_sent
        self._on_write_complete = on_write_complete
        self._device_name = device_name
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._outgoing: queue.Queue[
            tuple[int, Channel, int, int, bytes]
        ] = queue.Queue()
        self._transfer_id = 1
        self._diagnostic_lock = threading.Lock()
        self._diagnostics = BleDiagnostics()

    @property
    def running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self) -> None:
        if self.running:
            return
        while True:
            try:
                self._outgoing.get_nowait()
            except queue.Empty:
                break
        self._stop.clear()
        with self._diagnostic_lock:
            self._diagnostics = BleDiagnostics()
        self._thread = threading.Thread(
            target=self._thread_main, name="adelie-ble", daemon=True
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._outgoing.put((0, Channel.ADELIE, 0, 0, b""))
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None
        while True:
            try:
                self._outgoing.get_nowait()
            except queue.Empty:
                break

    def send(
        self,
        *,
        channel: Channel,
        destination: int,
        payload: bytes,
        flags: TransportFlag = TransportFlag.ACK_REQUIRED,
    ) -> int:
        transfer_id = self._transfer_id
        self._transfer_id = 1 if transfer_id == 0xFFFF else transfer_id + 1
        self._outgoing.put(
            (transfer_id, channel, destination, int(flags), bytes(payload))
        )
        return transfer_id

    def diagnostics(self) -> BleDiagnostics:
        with self._diagnostic_lock:
            values = self._diagnostics
            return BleDiagnostics(
                connected=values.connected,
                att_write_size=values.att_write_size,
                fragment_payload=values.fragment_payload,
                rx_notifications=values.rx_notifications,
                rx_frames=values.rx_frames,
                rx_messages=values.rx_messages,
                rx_bytes=values.rx_bytes,
                rx_decode_errors=values.rx_decode_errors,
                rx_reassembly_errors=values.rx_reassembly_errors,
                rx_address_errors=values.rx_address_errors,
                tx_messages=values.tx_messages,
                tx_fragments=values.tx_fragments,
                tx_bytes=values.tx_bytes,
                tx_write_errors=values.tx_write_errors,
                outgoing_queue=self._outgoing.qsize(),
                connected_since_ns=values.connected_since_ns,
                last_rx_ns=values.last_rx_ns,
                last_tx_ns=values.last_tx_ns,
                last_error=values.last_error,
            )

    def _update_diagnostics(self, **changes: object) -> None:
        with self._diagnostic_lock:
            current = self._diagnostics
            values = {
                field: getattr(current, field)
                for field in BleDiagnostics.__dataclass_fields__
            }
            for field, value in changes.items():
                if field.startswith("add_"):
                    target = field[4:]
                    values[target] = int(values[target]) + int(value)
                else:
                    values[field] = value
            self._diagnostics = BleDiagnostics(**values)

    def _thread_main(self) -> None:
        try:
            asyncio.run(self._run())
        except Exception as error:
            self._update_diagnostics(
                connected=False,
                last_error=f"{type(error).__name__}: {error}",
            )
            self._on_state("error", f"{type(error).__name__}: {error}")
            traceback.print_exc()
        finally:
            self._update_diagnostics(connected=False)

    async def _run(self) -> None:
        self._on_state("scanning", self._device_name)
        device = await BleakScanner.find_device_by_name(self._device_name, timeout=20)
        if device is None:
            self._on_state("error", f"{self._device_name} not found")
            return

        disconnected = asyncio.Event()

        def disconnected_callback(_client: BleakClient) -> None:
            disconnected.set()

        reassembler = Reassembler()

        def notification_callback(_sender: object, data: bytearray) -> None:
            received_ns = time.perf_counter_ns()
            self._update_diagnostics(
                add_rx_notifications=1,
                add_rx_bytes=len(data),
                last_rx_ns=received_ns,
            )
            try:
                frame = decode_frame(data)
                self._update_diagnostics(add_rx_frames=1)
            except Exception as error:
                self._update_diagnostics(
                    add_rx_decode_errors=1,
                    last_error=f"{type(error).__name__}: {error}",
                )
                self._on_state("warning", f"dropped BLE frame: {error}")
                return
            try:
                message = reassembler.accept(frame)
                if message is not None:
                    if message.destination != ADELIE_ADDRESS:
                        error = ValueError(
                            "BLE message is not addressed to Adelie"
                        )
                        self._update_diagnostics(
                            add_rx_address_errors=1,
                            last_error=f"ValueError: {error}",
                        )
                        self._on_state(
                            "warning", f"dropped BLE frame: {error}")
                        return
                    self._update_diagnostics(add_rx_messages=1)
                    self._on_message(message, received_ns)
            except Exception as error:
                self._update_diagnostics(
                    add_rx_reassembly_errors=1,
                    last_error=f"{type(error).__name__}: {error}",
                )
                self._on_state("warning", f"dropped BLE frame: {error}")

        async with BleakClient(
            device, timeout=20, disconnected_callback=disconnected_callback
        ) as client:
            characteristic = client.services.get_characteristic(RX_UUID)
            if characteristic is None:
                raise RuntimeError(
                    "Korora Adelie RX characteristic is missing")

            # BlueZ can otherwise leave this link at the 23 byte default ATT
            # MTU. With the 14 byte Fairy transport header that permits only
            # six payload bytes per frame and turns a small command into many
            # acknowledged connection events. Bleak documents this guarded
            # backend call as the Linux MTU acquisition workaround.
            acquire_mtu = getattr(
                getattr(client, "_backend", None), "_acquire_mtu", None
            )
            if callable(acquire_mtu):
                try:
                    await acquire_mtu()
                except Exception as error:
                    self._on_state(
                        "warning", f"ATT MTU acquisition failed: {error}"
                    )

            await client.start_notify(TX_UUID, notification_callback)

            reported_write_size = int(
                getattr(characteristic, "max_write_without_response_size", 20)
            )

            if reported_write_size == 20:
                deadline = asyncio.get_running_loop().time() + 2.0
                while (
                    reported_write_size == 20
                    and asyncio.get_running_loop().time() < deadline
                ):
                    await asyncio.sleep(0.1)
                    reported_write_size = int(
                        getattr(
                            characteristic,
                            "max_write_without_response_size",
                            20,
                        )
                    )

            # On Linux, BlueZ may leave the characteristic property at 20 even
            # after Bleak successfully acquires the real connection MTU.
            try:
                mtu_write_size = max(20, int(client.mtu_size) - 3)
            except Exception:
                mtu_write_size = 20

            # Korora is configured for an ATT MTU of 247, so its maximum attribute
            # value is 244 bytes.
            write_size = min(244, max(reported_write_size, mtu_write_size))

            fragment_payload = max(
                1,
                min(
                    write_size - 14,
                    180,
                ),
            )
            self._update_diagnostics(
                connected=True,
                att_write_size=int(write_size),
                fragment_payload=fragment_payload,
                connected_since_ns=time.perf_counter_ns(),
            )
            self._on_state(
                "connected",
                f"{device.name or str(device.address)} "
                f"ATT write={int(write_size)} fragment={fragment_payload}",
            )

            while not self._stop.is_set() and not disconnected.is_set():
                try:
                    (
                        transfer_id,
                        channel,
                        destination,
                        raw_flags,
                        payload,
                    ) = await asyncio.to_thread(self._outgoing.get, True, 0.2)
                except queue.Empty:
                    continue
                if self._stop.is_set():
                    break

                sent_ns = time.perf_counter_ns()
                if self._on_sent is not None:
                    self._on_sent(transfer_id, sent_ns)
                fragments = list(fragment_message(
                    channel=channel,
                    flags=TransportFlag(raw_flags),
                    source=ADELIE_ADDRESS,
                    destination=destination,
                    transfer_id=transfer_id,
                    payload=payload,
                    fragment_payload=fragment_payload,
                ))
                self._update_diagnostics(
                    add_tx_messages=1,
                    add_tx_fragments=len(fragments),
                    add_tx_bytes=sum(len(value) for value in fragments),
                    last_tx_ns=sent_ns,
                )
                for encoded in fragments:
                    # Adelie REQUIRE_RESPONSE is the end to end acknowledgement.
                    # An ATT Write Request duplicates it and costs one connection
                    # round trip for every transport fragment.
                    try:
                        await client.write_gatt_char(
                            characteristic, encoded, response=False
                        )
                    except Exception as error:
                        self._update_diagnostics(
                            add_tx_write_errors=1,
                            last_error=f"{type(error).__name__}: {error}",
                        )
                        raise
                if self._on_write_complete is not None:
                    self._on_write_complete(
                        transfer_id, time.perf_counter_ns())

            if disconnected.is_set():
                self._update_diagnostics(connected=False)
                self._on_state("disconnected", "Korora disconnected")
            else:
                await client.stop_notify(TX_UUID)
                self._update_diagnostics(connected=False)
                self._on_state("stopped", "BLE stopped")
