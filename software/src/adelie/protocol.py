from __future__ import annotations

from dataclasses import dataclass
import struct
from typing import Any, Iterable, Mapping

from .constants import (
    CommandFlag,
    Field,
    MessageKind,
    Opcode,
    RecordFlag,
    RecordType,
    Status,
    ValueType,
)

FAIRY_MAGIC = 0xFA13
FAIRY_VERSION = 3
FAIRY_HEADER = struct.Struct("<HBBHHIIQIHH")
FAIRY_HEADER_SIZE = FAIRY_HEADER.size

ADELIE_MAGIC = 0xAD1E
ADELIE_VERSION = 2
ADELIE_HEADER = struct.Struct("<HBBBBHHIIQI H".replace(" ", ""))
ADELIE_HEADER_SIZE = 32


@dataclass(frozen=True, slots=True)
class Tlv:
    tag: int
    value_type: ValueType
    value: Any
    raw: bytes


@dataclass(frozen=True, slots=True)
class FairyRecord:
    record_type: RecordType | int
    flags: RecordFlag
    record_id: int
    session_id: int
    timestamp_ticks: int
    clock_hz: int
    fields: tuple[Tlv, ...]
    raw_payload: bytes

    @property
    def field_map(self) -> dict[int, Any]:
        return {field.tag: field.value for field in self.fields}


@dataclass(frozen=True, slots=True)
class AdelieMessage:
    kind: MessageKind
    status: Status
    opcode: Opcode
    flags: CommandFlag
    command_id: int
    session_id: int
    execute_at_ticks: int
    deadline_ms: int
    fields: tuple[Tlv, ...]
    raw_payload: bytes


def _encode_value(value_type: ValueType, value: Any) -> bytes:
    match value_type:
        case ValueType.U8:
            return struct.pack("<B", int(value))
        case ValueType.U16:
            return struct.pack("<H", int(value))
        case ValueType.U32:
            return struct.pack("<I", int(value))
        case ValueType.U64:
            return struct.pack("<Q", int(value))
        case ValueType.I32:
            return struct.pack("<i", int(value))
        case ValueType.I64:
            return struct.pack("<q", int(value))
        case ValueType.F32:
            return struct.pack("<f", float(value))
        case ValueType.F64:
            return struct.pack("<d", float(value))
        case ValueType.BOOLEAN:
            return b"\x01" if value else b"\x00"
        case ValueType.BYTES:
            return bytes(value)
        case ValueType.STRING:
            return str(value).encode("utf-8")
    raise ValueError(f"unsupported TLV type {value_type}")


def encode_tlvs(
    fields: Iterable[tuple[int, ValueType, Any]] | Mapping[int, tuple[ValueType, Any]]
) -> bytes:
    items = fields.items() if isinstance(fields, Mapping) else fields
    encoded = bytearray()
    for item in items:
        if isinstance(fields, Mapping):
            tag, typed_value = item
            value_type, value = typed_value
        else:
            tag, value_type, value = item
        raw = _encode_value(ValueType(value_type), value)
        if len(raw) > 0xFF:
            raise ValueError(f"TLV field {tag} exceeds 255 bytes")
        encoded += struct.pack("<HBB", int(tag), int(value_type), len(raw))
        encoded += raw
    return bytes(encoded)


def _decode_value(value_type: ValueType, raw: bytes) -> Any:
    formats = {
        ValueType.U8: "<B",
        ValueType.U16: "<H",
        ValueType.U32: "<I",
        ValueType.U64: "<Q",
        ValueType.I32: "<i",
        ValueType.I64: "<q",
        ValueType.F32: "<f",
        ValueType.F64: "<d",
    }
    if value_type in formats:
        format_string = formats[value_type]
        if len(raw) != struct.calcsize(format_string):
            raise ValueError("TLV scalar has the wrong size")
        return struct.unpack(format_string, raw)[0]
    if value_type is ValueType.BOOLEAN:
        if len(raw) != 1 or raw[0] not in (0, 1):
            raise ValueError("invalid TLV Boolean")
        return bool(raw[0])
    if value_type is ValueType.STRING:
        return raw.decode("utf-8")
    if value_type is ValueType.BYTES:
        return raw
    raise ValueError(f"unsupported TLV type {value_type}")


def decode_tlvs(payload: bytes) -> tuple[Tlv, ...]:
    fields: list[Tlv] = []
    offset = 0
    while offset < len(payload):
        if offset + 4 > len(payload):
            raise ValueError("truncated TLV header")
        tag, raw_type, length = struct.unpack_from("<HBB", payload, offset)
        offset += 4
        if offset + length > len(payload):
            raise ValueError("truncated TLV value")
        raw = payload[offset : offset + length]
        offset += length
        value_type = ValueType(raw_type)
        fields.append(
            Tlv(
                tag=tag,
                value_type=value_type,
                value=_decode_value(value_type, raw),
                raw=raw,
            )
        )
    return tuple(fields)


def encode_adelie(
    *,
    kind: MessageKind,
    opcode: Opcode,
    command_id: int,
    session_id: int = 0,
    execute_at_ticks: int = 0,
    deadline_ms: int = 2000,
    status: Status = Status.OK,
    flags: CommandFlag = CommandFlag.REQUIRE_RESPONSE,
    fields: Iterable[tuple[int, ValueType, Any]] = (),
) -> bytes:
    payload = encode_tlvs(fields)
    if len(payload) > 256:
        raise ValueError("Adelie payload exceeds 256 bytes")
    header = bytearray(ADELIE_HEADER_SIZE)
    struct.pack_into(
        "<HBBBBHHIIQI H".replace(" ", ""),
        header,
        0,
        ADELIE_MAGIC,
        ADELIE_VERSION,
        ADELIE_HEADER_SIZE,
        int(kind),
        int(status),
        int(opcode),
        int(flags),
        command_id,
        session_id,
        execute_at_ticks,
        deadline_ms,
        len(payload),
    )
    return bytes(header) + payload


def decode_adelie(data: bytes) -> AdelieMessage:
    if len(data) < ADELIE_HEADER_SIZE:
        raise ValueError("short Adelie message")
    (
        magic,
        version,
        header_size,
        kind,
        status,
        opcode,
        flags,
        command_id,
        session_id,
        execute_at_ticks,
        deadline_ms,
        payload_length,
    ) = struct.unpack_from("<HBBBBHHIIQI H".replace(" ", ""), data)
    if magic != ADELIE_MAGIC or version != ADELIE_VERSION:
        raise ValueError("unsupported Adelie message")
    if header_size != ADELIE_HEADER_SIZE:
        raise ValueError("invalid Adelie header size")
    if len(data) != header_size + payload_length:
        raise ValueError("Adelie payload length mismatch")
    payload = data[header_size:]
    return AdelieMessage(
        kind=MessageKind(kind),
        status=Status(status),
        opcode=Opcode(opcode),
        flags=CommandFlag(flags),
        command_id=command_id,
        session_id=session_id,
        execute_at_ticks=execute_at_ticks,
        deadline_ms=deadline_ms,
        fields=decode_tlvs(payload),
        raw_payload=payload,
    )


def decode_fairy(data: bytes) -> FairyRecord:
    if len(data) < FAIRY_HEADER_SIZE:
        raise ValueError("short Fairy record")
    (
        magic,
        version,
        header_size,
        record_type,
        flags,
        record_id,
        session_id,
        timestamp_ticks,
        clock_hz,
        payload_length,
        reserved,
    ) = FAIRY_HEADER.unpack_from(data)
    if magic != FAIRY_MAGIC or version != FAIRY_VERSION:
        raise ValueError("unsupported Fairy record")
    if header_size != FAIRY_HEADER_SIZE or reserved != 0:
        raise ValueError("invalid Fairy header")
    if len(data) != header_size + payload_length:
        raise ValueError("Fairy payload length mismatch")
    payload = data[header_size:]
    try:
        decoded_type: RecordType | int = RecordType(record_type)
    except ValueError:
        decoded_type = record_type
    return FairyRecord(
        record_type=decoded_type,
        flags=RecordFlag(flags),
        record_id=record_id,
        session_id=session_id,
        timestamp_ticks=timestamp_ticks,
        clock_hz=clock_hz,
        fields=decode_tlvs(payload),
        raw_payload=payload,
    )


def field_value(record: FairyRecord | AdelieMessage, tag: Field | int, default: Any = None) -> Any:
    wanted = int(tag)
    for field in record.fields:
        if field.tag == wanted:
            return field.value
    return default
