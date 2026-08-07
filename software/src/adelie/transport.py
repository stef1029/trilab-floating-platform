from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Iterable

from .constants import Channel, TransportFlag

MAGIC = 0xA7C3
VERSION = 1
HEADER = struct.Struct("<HBBBBBBB BH".replace(" ", ""))
HEADER_SIZE = 12
CRC_SIZE = 2
MAX_RAW_FRAME = 255
MAX_FRAGMENT = MAX_RAW_FRAME - HEADER_SIZE - CRC_SIZE
MAX_MESSAGE = 512


def crc16_ccitt(data: bytes | bytearray | memoryview) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (
                crc << 1) & 0xFFFF
    return crc


@dataclass(frozen=True, slots=True)
class Frame:
    channel: Channel
    flags: TransportFlag
    source: int
    destination: int
    fragment_index: int
    fragment_count: int
    transfer_id: int
    payload: bytes


@dataclass(frozen=True, slots=True)
class Message:
    channel: Channel
    flags: TransportFlag
    source: int
    destination: int
    transfer_id: int
    payload: bytes


def encode_frame(frame: Frame) -> bytes:
    if not 0 <= frame.source <= 0xFF or not 0 <= frame.destination <= 0xFF:
        raise ValueError("transport address is outside one byte")
    if not 0 <= frame.transfer_id <= 0xFFFF:
        raise ValueError("transfer id is outside two bytes")
    if not 1 <= frame.fragment_count <= 0xFF:
        raise ValueError("invalid fragment count")
    if not 0 <= frame.fragment_index < frame.fragment_count:
        raise ValueError("invalid fragment index")
    if len(frame.payload) > MAX_FRAGMENT:
        raise ValueError("transport fragment is too large")

    header = struct.pack(
        "<HBBBBBBBBH",
        MAGIC,
        VERSION,
        int(frame.channel),
        int(frame.flags),
        frame.source,
        frame.destination,
        frame.fragment_index,
        frame.fragment_count,
        len(frame.payload),
        frame.transfer_id,
    )
    body = header + frame.payload
    return body + struct.pack("<H", crc16_ccitt(body))


def decode_frame(data: bytes | bytearray | memoryview) -> Frame:
    raw = bytes(data)
    if len(raw) < HEADER_SIZE + CRC_SIZE or len(raw) > MAX_RAW_FRAME:
        raise ValueError("invalid transport frame length")

    (
        magic,
        version,
        channel,
        flags,
        source,
        destination,
        fragment_index,
        fragment_count,
        payload_length,
        transfer_id,
    ) = struct.unpack_from("<HBBBBBBBBH", raw)

    if magic != MAGIC or version != VERSION:
        raise ValueError("invalid transport magic or version")
    if len(raw) != HEADER_SIZE + payload_length + CRC_SIZE:
        raise ValueError("transport payload length mismatch")
    if fragment_count == 0 or fragment_index >= fragment_count:
        raise ValueError("invalid transport fragment fields")
    if crc16_ccitt(raw[:-2]) != struct.unpack_from("<H", raw, len(raw) - 2)[0]:
        raise ValueError("transport CRC mismatch")

    return Frame(
        channel=Channel(channel),
        flags=TransportFlag(flags),
        source=source,
        destination=destination,
        fragment_index=fragment_index,
        fragment_count=fragment_count,
        transfer_id=transfer_id,
        payload=raw[HEADER_SIZE:-2],
    )


def fragment_message(
    *,
    channel: Channel,
    flags: TransportFlag,
    source: int,
    destination: int,
    transfer_id: int,
    payload: bytes,
    fragment_payload: int,
) -> Iterable[bytes]:
    if len(payload) > MAX_MESSAGE:
        raise ValueError(f"message exceeds {MAX_MESSAGE} bytes")
    fragment_payload = max(1, min(fragment_payload, MAX_FRAGMENT))
    count = max(1, (len(payload) + fragment_payload - 1) // fragment_payload)
    if count > 0xFF:
        raise ValueError("message requires too many fragments")

    for index in range(count):
        start = index * fragment_payload
        chunk = payload[start: start + fragment_payload]
        fragment_flags = flags
        if index == 0:
            fragment_flags |= TransportFlag.FIRST_FRAGMENT
        if index + 1 == count:
            fragment_flags |= TransportFlag.LAST_FRAGMENT
        yield encode_frame(
            Frame(
                channel=channel,
                flags=fragment_flags,
                source=source,
                destination=destination,
                fragment_index=index,
                fragment_count=count,
                transfer_id=transfer_id,
                payload=chunk,
            )
        )


class Reassembler:
    def __init__(self) -> None:
        self._active: dict[tuple[int, int, Channel], bytearray] = {}
        self._headers: dict[tuple[int, int, Channel], Frame] = {}
        self._next_index: dict[tuple[int, int, Channel], int] = {}

    def accept(self, frame: Frame) -> Message | None:
        key = (frame.source, frame.transfer_id, frame.channel)
        if frame.fragment_index == 0:
            self._active[key] = bytearray()
            self._headers[key] = frame
            self._next_index[key] = 0

        payload = self._active.get(key)
        first = self._headers.get(key)
        expected = self._next_index.get(key)
        if (
            payload is None
            or first is None
            or expected is None
            or frame.fragment_count != first.fragment_count
            or frame.destination != first.destination
            or frame.fragment_index != expected
        ):
            self._active.pop(key, None)
            self._headers.pop(key, None)
            self._next_index.pop(key, None)
            raise ValueError("out of order transport fragment")

        payload.extend(frame.payload)
        if len(payload) > MAX_MESSAGE:
            self._active.pop(key, None)
            self._headers.pop(key, None)
            self._next_index.pop(key, None)
            raise ValueError("reassembled message is too large")
        self._next_index[key] = expected + 1
        if expected + 1 != frame.fragment_count:
            return None

        first = self._headers.pop(key)
        self._active.pop(key)
        self._next_index.pop(key)
        return Message(
            channel=first.channel,
            flags=first.flags,
            source=first.source,
            destination=first.destination,
            transfer_id=first.transfer_id,
            payload=bytes(payload),
        )
