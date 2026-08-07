from __future__ import annotations

import base64
from dataclasses import asdict
import json
from pathlib import Path
import threading
import time
from typing import Any

from .protocol import AdelieMessage, FairyRecord, Tlv
from .transport import Message

LOG_SCHEMA = "fairy_log"
LOG_VERSION = 1


def _json_value(value: Any) -> Any:
    if isinstance(value, bytes):
        return {"base64": base64.b64encode(value).decode("ascii")}
    return value


def _field_json(field: Tlv) -> dict[str, Any]:
    return {
        "tag": field.tag,
        "type": field.value_type.name.lower(),
        "value": _json_value(field.value),
    }


def _record_type_name(record: FairyRecord) -> str:
    if hasattr(record.record_type, "name"):
        return str(record.record_type.name).lower()
    return f"record_0x{int(record.record_type):04x}"


class LogRecorder:
    """A single writer prevents interleaved or partly written log lines."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._handle = None
        self.path: Path | None = None
        self.session_id = 0

    @property
    def active(self) -> bool:
        return self._handle is not None

    def start(self, path: Path, session_id: int, metadata: dict[str, Any]) -> None:
        with self._lock:
            if self._handle is not None:
                raise RuntimeError("a recording is already active")
            path.parent.mkdir(parents=True, exist_ok=True)
            self._handle = path.open("x", encoding="utf-8", buffering=1)
            self.path = path
            self.session_id = session_id
            self._write_locked(
                {
                    "schema": LOG_SCHEMA,
                    "version": LOG_VERSION,
                    "kind": "header",
                    "created_unix_ns": time.time_ns(),
                    "session_id": session_id,
                    "metadata": metadata,
                }
            )

    def record(
        self,
        *,
        received_monotonic_ns: int,
        transport_message: Message,
        record: FairyRecord,
    ) -> None:
        with self._lock:
            if self._handle is None:
                return
            self._write_locked(
                {
                    "kind": "record",
                    "received_monotonic_ns": received_monotonic_ns,
                    "source": transport_message.source,
                    "destination": transport_message.destination,
                    "transfer_id": transport_message.transfer_id,
                    "record_type": _record_type_name(record),
                    "record_type_value": int(record.record_type),
                    "flags": int(record.flags),
                    "record_id": record.record_id,
                    "session_id": record.session_id,
                    "timestamp_ticks": record.timestamp_ticks,
                    "clock_hz": record.clock_hz,
                    "fields": [_field_json(field) for field in record.fields],
                    "raw_message_base64": base64.b64encode(
                        transport_message.payload
                    ).decode("ascii"),
                }
            )

    def command_queued(
        self,
        *,
        queued_monotonic_ns: int,
        transfer_id: int,
        command_id: int,
        opcode: int,
        opcode_name: str,
        destination: int,
        session_id: int,
        execute_at_ticks: int,
        deadline_ms: int,
        fields: list[tuple[int, Any, Any]],
    ) -> None:
        with self._lock:
            if self._handle is None:
                return
            encoded_fields = []
            for tag, value_type, value in fields:
                encoded_fields.append(
                    {
                        "tag": int(tag),
                        "type": getattr(value_type, "name", str(value_type)).lower(),
                        "value": _json_value(value),
                    }
                )
            self._write_locked(
                {
                    "kind": "command_queued",
                    "queued_monotonic_ns": queued_monotonic_ns,
                    "transfer_id": transfer_id,
                    "command_id": command_id,
                    "opcode": opcode_name.lower(),
                    "opcode_value": opcode,
                    "destination": destination,
                    "session_id": session_id,
                    "execute_at_ticks": execute_at_ticks,
                    "deadline_ms": deadline_ms,
                    "fields": encoded_fields,
                }
            )

    def transport_sent(self, *, transfer_id: int, sent_monotonic_ns: int) -> None:
        with self._lock:
            if self._handle is None:
                return
            self._write_locked(
                {
                    "kind": "transport_sent",
                    "transfer_id": transfer_id,
                    "sent_monotonic_ns": sent_monotonic_ns,
                }
            )

    def transport_write_complete(
        self, *, transfer_id: int, completed_monotonic_ns: int
    ) -> None:
        with self._lock:
            if self._handle is None:
                return
            self._write_locked(
                {
                    "kind": "transport_write_complete",
                    "transfer_id": transfer_id,
                    "completed_monotonic_ns": completed_monotonic_ns,
                }
            )

    def command_response(
        self,
        *,
        received_monotonic_ns: int,
        transport_message: Message,
        response: AdelieMessage,
    ) -> None:
        with self._lock:
            if self._handle is None:
                return
            self._write_locked(
                {
                    "kind": "command_response",
                    "received_monotonic_ns": received_monotonic_ns,
                    "source": transport_message.source,
                    "destination": transport_message.destination,
                    "transfer_id": transport_message.transfer_id,
                    "command_id": response.command_id,
                    "opcode": response.opcode.name.lower(),
                    "opcode_value": int(response.opcode),
                    "status": response.status.name.lower(),
                    "status_value": int(response.status),
                    "session_id": response.session_id,
                    "fields": [_field_json(field) for field in response.fields],
                }
            )

    def marker(self, kind: str, **values: Any) -> None:
        with self._lock:
            if self._handle is None:
                return
            self._write_locked(
                {
                    "kind": kind,
                    "received_monotonic_ns": time.perf_counter_ns(),
                    **values,
                }
            )

    def stop(self, reason: str) -> Path | None:
        with self._lock:
            if self._handle is None:
                return self.path
            self._write_locked(
                {
                    "kind": "footer",
                    "closed_unix_ns": time.time_ns(),
                    "reason": reason,
                    "session_id": self.session_id,
                }
            )
            self._handle.flush()
            self._handle.close()
            self._handle = None
            return self.path

    def _write_locked(self, value: dict[str, Any]) -> None:
        assert self._handle is not None
        self._handle.write(
            json.dumps(value, separators=(",", ":"), ensure_ascii=False) + "\n"
        )
        self._handle.flush()
