from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any

from adelie.constants import (
    ADELIE_ADDRESS,
    COMMON_TIMER_HZ,
    FAIRY_ADDRESS_BASE,
    GALAPAGOS_ADDRESS,
    KORORA_ADDRESS,
    Opcode,
    Status,
)

from .logio import command_fields_by_name, fields_by_name, read_log


DEFAULT_COMMAND_DEADLINE_MS = 2_000
RESPONSE_DELIVERY_GRACE_MS = 2_000


NODE_COLUMNS = [
    "source_address",
    "source_node",
    "source_label",
    "source_fairy_index",
    "destination_address",
    "destination_node",
    "destination_label",
    "destination_fairy_index",
]


RECORD_COLUMNS = [
    "received_monotonic_ns",
    "source",
    "destination",
    "transfer_id",
    "record_type",
    "record_type_value",
    "flags",
    "record_id",
    "session_id",
    "timestamp_ticks",
    "clock_hz",
    *NODE_COLUMNS,
    "fields_json",
]

SPECIAL_COLUMNS: dict[str, list[str]] = {
    "sync_pairs": [
        "received_monotonic_ns",
        "source",
        *NODE_COLUMNS,
        "record_id",
        "session_id",
        "timestamp_ticks",
        "clock_hz",
        "sequence",
        "requested_ticks",
        "actual_ticks",
        "reference_ticks",
        "duration_us",
        "state",
    ],
    "sync_quality": [
        "received_monotonic_ns",
        "source",
        *NODE_COLUMNS,
        "flags",
        "timestamp_ticks",
        "rms_ns",
        "skew_ppb",
        "model_points",
        "model_generation",
        "interval_error_ppb",
        "dropped_records",
        "transport_errors",
    ],
    "ttl": [
        "received_monotonic_ns",
        "source",
        *NODE_COLUMNS,
        "record_type",
        "record_id",
        "session_id",
        "timestamp_ticks",
        "clock_hz",
        "sequence",
        "requested_ticks",
        "actual_ticks",
        "reference_ticks",
        "duration_us",
        "value",
        "status",
    ],
    "events": [
        "received_monotonic_ns",
        "source",
        *NODE_COLUMNS,
        "record_type",
        "record_id",
        "session_id",
        "timestamp_ticks",
        "clock_hz",
        "sequence",
        "reference_ticks",
        "state",
        "channel",
        "value",
        "command_id",
        "operation",
        "status",
    ],
    "health": [
        "received_monotonic_ns",
        "source",
        *NODE_COLUMNS,
        "record_type",
        "uptime_ms",
        "queue_depth",
        "queue_capacity",
        "dropped_records",
        "transport_errors",
        "duplicate_frames",
        "timeout_count",
        "retry_count",
        "transport_decode_errors",
        "transport_reassembly_errors",
        "transport_transmit_errors",
        "ttl_capture_count",
        "ttl_capture_drops",
        "rssi_dbm",
    ],
    "transport_timing": [
        "received_monotonic_ns",
        "source",
        *NODE_COLUMNS,
        "record_id",
        "session_id",
        "timestamp_ticks",
        "clock_hz",
        "related_transfer_id",
        "command_id",
        "operation",
        "gateway_receive_ticks",
        "response_queued_ticks",
        "transmit_start_ticks",
        "transmit_complete_ticks",
        "fragment_count",
        "att_mtu",
    ],
}


COMMAND_COLUMNS = [
    "command_id",
    "transfer_id",
    "opcode",
    "opcode_value",
    "session_id",
    "deadline_ms",
    "destination_address",
    "destination_node",
    "destination_label",
    "destination_fairy_index",
    "queued_monotonic_ns",
    "sent_monotonic_ns",
    "write_completed_monotonic_ns",
    "response_received_monotonic_ns",
    "queue_delay_us",
    "gatt_write_duration_us",
    "write_complete_to_response_us",
    "queue_to_response_rtt_us",
    "transport_rtt_us",
    "response_source_address",
    "response_source_node",
    "response_source_label",
    "response_source_fairy_index",
    "status",
    "status_value",
    "response_missing",
    "response_match_method",
    "response_match_reason",
    "response_candidate_count",
    "response_duplicate_count",
    "response_timing_valid",
    "result_count",
    "result_sources",
    "clock_t1_ns",
    "clock_t2_ticks",
    "clock_t3_ticks",
    "clock_full_exchange_us",
    "clock_server_processing_us",
    "clock_network_rtt_us",
    "korora_receive_ticks",
    "korora_response_queued_ticks",
    "korora_tx_start_ticks",
    "korora_tx_complete_ticks",
    "korora_receive_to_queue_us",
    "korora_queue_to_tx_start_us",
    "korora_notify_duration_us",
    "korora_receive_to_tx_complete_us",
    "korora_fragment_count",
    "korora_att_mtu",
    "korora_timing_missing",
    "request_fields_json",
    "response_fields_json",
]


COMMAND_RESULT_COLUMNS = [
    "command_id",
    "operation",
    "operation_value",
    "status",
    "status_value",
    "source_address",
    "source_node",
    "source_label",
    "source_fairy_index",
    "session_id",
    "record_id",
    "record_timestamp_ticks",
    "reference_ticks",
    "result_received_monotonic_ns",
    "command_sent_monotonic_ns",
    "result_receive_latency_us",
]


HOST_TRANSPORT_COLUMNS = [
    "received_monotonic_ns",
    "sample_ns",
    "ble_connected",
    "att_write_size",
    "fragment_payload",
    "rx_notifications",
    "rx_frames",
    "rx_messages",
    "rx_bytes",
    "rx_decode_errors",
    "rx_reassembly_errors",
    "rx_address_errors",
    "tx_messages",
    "tx_fragments",
    "tx_bytes",
    "tx_write_errors",
    "outgoing_queue",
    "last_rx_age_ms",
    "last_tx_age_ms",
    "last_error",
    "clock_valid",
    "clock_points",
    "clock_rms_us",
    "clock_median_rtt_us",
    "clock_skew_ppm",
    "clock_exchange_count",
    "clock_last_exchange_ns",
    "clock_last_rtt_us",
    "korora_rs485_errors",
    "korora_rs485_retries",
    "korora_rs485_timeouts",
    "korora_rs485_decode_errors",
    "korora_rs485_reassembly_errors",
    "korora_rs485_transmit_errors",
    "korora_ble_dropped",
    "galapagos_ttl_generated",
    "korora_ttl_captured_records",
    "korora_ttl_capture_edges",
    "korora_ttl_capture_drops",
]


MARKER_COLUMNS = [
    "received_monotonic_ns",
    "kind",
    "values_json",
]


def _node_map(header: dict[str, Any] | None) -> dict[int, dict[str, Any]]:
    nodes: dict[int, dict[str, Any]] = {
        KORORA_ADDRESS: {"node": "korora", "label": "korora", "fairy_index": ""},
        GALAPAGOS_ADDRESS: {
            "node": "galapagos",
            "label": "galapagos",
            "fairy_index": "",
        },
        ADELIE_ADDRESS: {"node": "adelie", "label": "adelie", "fairy_index": ""},
    }
    metadata = (header or {}).get("metadata", {})
    for assignment in metadata.get("assignments", []):
        try:
            fairy_index = int(assignment["fairy_number"])
        except (KeyError, TypeError, ValueError):
            continue
        address = FAIRY_ADDRESS_BASE + fairy_index
        node = f"fairy{fairy_index}"
        nodes[address] = {
            "node": node,
            "label": str(assignment.get("label") or node),
            "fairy_index": fairy_index,
        }
    return nodes


def _describe_address(
    address: Any, nodes: dict[int, dict[str, Any]], prefix: str
) -> dict[str, Any]:
    if address in (None, ""):
        return {
            f"{prefix}_address": "",
            f"{prefix}_node": "",
            f"{prefix}_label": "",
            f"{prefix}_fairy_index": "",
        }
    numeric = int(address)
    description = nodes.get(
        numeric,
        {"node": f"node_0x{numeric:02x}", "label": "", "fairy_index": ""},
    )
    return {
        f"{prefix}_address": numeric,
        f"{prefix}_node": description["node"],
        f"{prefix}_label": description["label"],
        f"{prefix}_fairy_index": description["fairy_index"],
    }


def _open_writer(path: Path, columns: list[str]) -> tuple[Any, csv.DictWriter]:
    handle = path.open("w", newline="", encoding="utf-8")
    writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
    writer.writeheader()
    return handle, writer


def _difference_us(later: Any, earlier: Any) -> float | str:
    if later in (None, "") or earlier in (None, ""):
        return ""
    return (int(later) - int(earlier)) / 1000.0


def _tick_difference_us(later: Any, earlier: Any) -> float | str:
    if later in (None, "") or earlier in (None, ""):
        return ""
    return (int(later) - int(earlier)) * 1_000_000 / COMMON_TIMER_HZ


def _enum_name(enum_type: Any, value: Any) -> str:
    try:
        return enum_type(int(value)).name.lower()
    except (TypeError, ValueError):
        return ""


def _integer_or_none(value: Any) -> int | None:
    if value in (None, ""):
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _opcode_matches(request: dict[str, Any], response: dict[str, Any]) -> bool:
    request_value = _integer_or_none(request.get("opcode_value"))
    response_value = _integer_or_none(response.get("opcode_value"))
    if request_value is not None and response_value is not None:
        return request_value == response_value
    request_name = str(request.get("opcode") or "").lower()
    response_name = str(response.get("opcode") or "").lower()
    return bool(request_name) and request_name == response_name


def _group_timestamps(
    items: dict[int, list[int]], transfer_id: int, not_before_ns: int
) -> int | None:
    """Claim the first timestamp for this occurrence of a transport ID."""
    candidates = items.get(transfer_id, [])
    while candidates and candidates[0] < not_before_ns:
        candidates.pop(0)
    return candidates.pop(0) if candidates else None


def _match_response(
    request: dict[str, Any],
    not_before_ns: int,
    responses_by_command: dict[int, list[dict[str, Any]]],
    responses_by_transfer: dict[int, list[dict[str, Any]]],
    consumed_responses: set[int],
) -> tuple[dict[str, Any] | None, str, str, int, int]:
    """Return one identity and chronology checked response for a request."""
    command_id = _integer_or_none(request.get("command_id"))
    transfer_id = int(request["transfer_id"])
    if command_id is not None:
        candidates = list(responses_by_command.get(command_id, []))
        method = "command_id"
    else:
        # Compatibility only for very early logs without application command IDs.
        candidates = list(responses_by_transfer.get(transfer_id, []))
        method = "legacy_transfer_id"

    candidate_count = len(candidates)
    available = [item for item in candidates if id(
        item) not in consumed_responses]
    if not available:
        return None, method, "missing", candidate_count, 0

    opcode_matches = [
        item for item in available if _opcode_matches(request, item)]
    if not opcode_matches:
        return None, method, "opcode_mismatch", candidate_count, 0

    identity_matches = [
        item
        for item in opcode_matches
        if _integer_or_none(item.get("transfer_id")) == transfer_id
    ]
    if not identity_matches:
        return None, method, "transfer_id_mismatch", candidate_count, 0

    chronological = [
        item
        for item in identity_matches
        if int(item.get("received_monotonic_ns", -1)) >= not_before_ns
    ]
    if not chronological:
        return None, method, "response_before_send", candidate_count, 0

    chronological.sort(key=lambda item: int(item["received_monotonic_ns"]))
    response = chronological[0]
    consumed_responses.add(id(response))
    return (
        response,
        method,
        "matched",
        candidate_count,
        len(chronological) - 1,
    )


def _matching_timing(
    response: dict[str, Any] | None,
    sent_ns: int | None,
    deadline_ms: int,
) -> tuple[bool, str]:
    if response is None:
        return False, "no_matched_response"
    if sent_ns is None:
        return False, "send_timestamp_missing"
    received_ns = int(response["received_monotonic_ns"])
    if received_ns < sent_ns:
        return False, "response_before_send"
    maximum_ns = (
        deadline_ms + RESPONSE_DELIVERY_GRACE_MS
    ) * 1_000_000
    if received_ns - sent_ns > maximum_ns:
        return False, "response_after_deadline"
    return True, ""


def _write_command_outputs(
    output_directory: Path,
    outputs: dict[str, Path],
    nodes: dict[int, dict[str, Any]],
    queued: list[dict[str, Any]],
    sent: dict[int, list[int]],
    write_completed: dict[int, list[int]],
    responses: list[dict[str, Any]],
    results: list[dict[str, Any]],
    transport_timings: list[dict[str, Any]],
) -> None:
    responses_by_transfer: dict[int, list[dict[str, Any]]] = {}
    responses_by_command: dict[int, list[dict[str, Any]]] = {}
    for item in responses:
        transfer_id = _integer_or_none(item.get("transfer_id"))
        command_id = _integer_or_none(item.get("command_id"))
        if transfer_id is not None:
            responses_by_transfer.setdefault(transfer_id, []).append(item)
        if command_id is not None:
            responses_by_command.setdefault(command_id, []).append(item)
    for grouped in (responses_by_transfer, responses_by_command):
        for candidates in grouped.values():
            candidates.sort(key=lambda item: int(
                item["received_monotonic_ns"]))
    results_by_command: dict[int, list[dict[str, Any]]] = {}
    for item in results:
        command_id = item.get("command_id")
        if command_id not in (None, ""):
            results_by_command.setdefault(int(command_id), []).append(item)
    timings_by_transfer: dict[int, list[dict[str, Any]]] = {}
    for item in transport_timings:
        transfer_id = _integer_or_none(item.get("related_transfer_id"))
        if transfer_id is not None:
            timings_by_transfer.setdefault(transfer_id, []).append(item)
    for candidates in timings_by_transfer.values():
        candidates.sort(key=lambda item: int(item["received_monotonic_ns"]))

    command_rows: list[dict[str, Any]] = []
    queued_by_command: dict[int, dict[str, Any]] = {}
    sent_by_command: dict[int, int] = {}
    consumed_responses: set[int] = set()
    for request in sorted(queued, key=lambda item: int(item["queued_monotonic_ns"])):
        transfer_id = int(request["transfer_id"])
        command_id = int(request["command_id"])
        queued_by_command[command_id] = request
        queued_ns = int(request["queued_monotonic_ns"])
        sent_ns = _group_timestamps(sent, transfer_id, queued_ns)
        not_before_ns = sent_ns if sent_ns is not None else queued_ns
        write_completed_ns = _group_timestamps(
            write_completed, transfer_id, not_before_ns
        )
        if sent_ns is not None:
            sent_by_command[command_id] = sent_ns
        response, match_method, match_reason, candidate_count, duplicate_count = (
            _match_response(
                request,
                not_before_ns,
                responses_by_command,
                responses_by_transfer,
                consumed_responses,
            )
        )
        deadline_ms = int(
            request.get("deadline_ms", DEFAULT_COMMAND_DEADLINE_MS)
            or DEFAULT_COMMAND_DEADLINE_MS
        )
        timing_valid, timing_reason = _matching_timing(
            response, sent_ns, deadline_ms
        )
        request_fields = command_fields_by_name(request)
        response_fields = command_fields_by_name(response or {})
        received_ns = (response or {}).get("received_monotonic_ns")
        destination = int(request["destination"])
        result_items = results_by_command.get(command_id, [])
        timing_candidates = [
            item
            for item in timings_by_transfer.get(transfer_id, [])
            if int(item["received_monotonic_ns"]) >= not_before_ns
            and _integer_or_none(item.get("command_id")) in (None, command_id)
        ]
        timing = timing_candidates[0] if timing_candidates else {}
        gateway_receive = timing.get("gateway_receive_ticks", "")
        response_queued = timing.get("response_queued_ticks", "")
        transmit_start = timing.get("transmit_start_ticks", "")
        transmit_complete = timing.get("transmit_complete_ticks", "")
        row: dict[str, Any] = {
            "command_id": command_id,
            "transfer_id": transfer_id,
            "opcode": request.get("opcode", ""),
            "opcode_value": request.get("opcode_value", ""),
            "session_id": request.get("session_id", ""),
            "deadline_ms": deadline_ms,
            **_describe_address(destination, nodes, "destination"),
            "queued_monotonic_ns": queued_ns,
            "sent_monotonic_ns": sent_ns or "",
            "write_completed_monotonic_ns": write_completed_ns or "",
            "response_received_monotonic_ns": received_ns or "",
            "queue_delay_us": _difference_us(sent_ns, queued_ns),
            "gatt_write_duration_us": _difference_us(
                write_completed_ns, sent_ns
            ),
            "write_complete_to_response_us": _difference_us(
                received_ns, write_completed_ns
            ) if timing_valid else "",
            "queue_to_response_rtt_us": (
                _difference_us(received_ns, queued_ns) if timing_valid else ""
            ),
            "transport_rtt_us": (
                _difference_us(received_ns, sent_ns) if timing_valid else ""
            ),
            "status": (response or {}).get("status", ""),
            "status_value": (response or {}).get("status_value", ""),
            "response_missing": response is None,
            "response_match_method": match_method,
            "response_match_reason": (
                timing_reason if response is not None and not timing_valid
                else match_reason
            ),
            "response_candidate_count": candidate_count,
            "response_duplicate_count": duplicate_count,
            "response_timing_valid": timing_valid,
            "result_count": len(result_items),
            "result_sources": "|".join(
                sorted({str(item["source_node"]) for item in result_items})
            ),
            "clock_t1_ns": request_fields.get("clock_t1_ns", ""),
            "clock_t2_ticks": response_fields.get("clock_t2_ticks", ""),
            "clock_t3_ticks": response_fields.get("clock_t3_ticks", ""),
            "korora_receive_ticks": gateway_receive,
            "korora_response_queued_ticks": response_queued,
            "korora_tx_start_ticks": transmit_start,
            "korora_tx_complete_ticks": transmit_complete,
            "korora_receive_to_queue_us": _tick_difference_us(
                response_queued, gateway_receive
            ),
            "korora_queue_to_tx_start_us": _tick_difference_us(
                transmit_start, response_queued
            ),
            "korora_notify_duration_us": _tick_difference_us(
                transmit_complete, transmit_start
            ),
            "korora_receive_to_tx_complete_us": _tick_difference_us(
                transmit_complete, gateway_receive
            ),
            "korora_fragment_count": timing.get("fragment_count", ""),
            "korora_att_mtu": timing.get("att_mtu", ""),
            "korora_timing_missing": not bool(timing),
            "request_fields_json": json.dumps(
                request_fields, separators=(",", ":")
            ),
            "response_fields_json": json.dumps(
                response_fields, separators=(",", ":")
            ),
        }
        if response is not None:
            row.update(_describe_address(response.get(
                "source"), nodes, "response_source"))
        else:
            row.update(_describe_address(None, nodes, "response_source"))
        t1 = row["clock_t1_ns"]
        t2 = row["clock_t2_ticks"]
        t3 = row["clock_t3_ticks"]
        if timing_valid and t1 not in (None, "") and received_ns not in (None, ""):
            full_us = (int(received_ns) - int(t1)) / 1000.0
            row["clock_full_exchange_us"] = full_us
            if t2 not in (None, "") and t3 not in (None, ""):
                processing_us = (int(t3) - int(t2)) * \
                    1_000_000 / COMMON_TIMER_HZ
                row["clock_server_processing_us"] = processing_us
                row["clock_network_rtt_us"] = full_us - processing_us
        command_rows.append(row)

    commands_path = output_directory / "commands.csv"
    with commands_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=COMMAND_COLUMNS, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(command_rows)
    outputs["commands"] = commands_path

    result_rows: list[dict[str, Any]] = []
    for result in results:
        command_id_value = result.get("command_id")
        command_id = (
            int(command_id_value) if command_id_value not in (None, "") else None
        )
        request = queued_by_command.get(
            command_id) if command_id is not None else None
        sent_ns = sent_by_command.get(
            command_id) if command_id is not None else None
        operation_value = result.get("operation", "")
        status_value = result.get("status", "")
        result_rows.append(
            {
                "command_id": command_id if command_id is not None else "",
                "operation": _enum_name(Opcode, operation_value),
                "operation_value": operation_value,
                "status": _enum_name(Status, status_value),
                "status_value": status_value,
                "source_address": result["source_address"],
                "source_node": result["source_node"],
                "source_label": result["source_label"],
                "source_fairy_index": result["source_fairy_index"],
                "session_id": result.get("session_id", ""),
                "record_id": result.get("record_id", ""),
                "record_timestamp_ticks": result.get("timestamp_ticks", ""),
                "reference_ticks": result.get("reference_ticks", ""),
                "result_received_monotonic_ns": result.get(
                    "received_monotonic_ns", ""
                ),
                "command_sent_monotonic_ns": sent_ns or "",
                "result_receive_latency_us": _difference_us(
                    result.get("received_monotonic_ns"), sent_ns
                ),
            }
        )

    results_path = output_directory / "command_results.csv"
    with results_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=COMMAND_RESULT_COLUMNS, extrasaction="ignore"
        )
        writer.writeheader()
        writer.writerows(result_rows)
    outputs["command_results"] = results_path


def parse(log_path: Path, output_directory: Path) -> dict[str, Path]:
    output_directory.mkdir(parents=True, exist_ok=True)
    handles: list[Any] = []
    outputs: dict[str, Path] = {}

    records_path = output_directory / "records.csv"
    records_handle, records_writer = _open_writer(records_path, RECORD_COLUMNS)
    handles.append(records_handle)
    outputs["records"] = records_path

    writers: dict[str, csv.DictWriter] = {}
    for name, columns in SPECIAL_COLUMNS.items():
        path = output_directory / f"{name}.csv"
        handle, writer = _open_writer(path, columns)
        handles.append(handle)
        writers[name] = writer
        outputs[name] = path

    host_transport_path = output_directory / "host_transport_health.csv"
    host_transport_handle, host_transport_writer = _open_writer(
        host_transport_path, HOST_TRANSPORT_COLUMNS
    )
    handles.append(host_transport_handle)
    outputs["host_transport_health"] = host_transport_path

    markers_path = output_directory / "markers.csv"
    markers_handle, markers_writer = _open_writer(markers_path, MARKER_COLUMNS)
    handles.append(markers_handle)
    outputs["markers"] = markers_path

    metadata: dict[str, Any] | None = None
    footer: dict[str, Any] | None = None
    nodes = _node_map(None)
    queued_commands: list[dict[str, Any]] = []
    sent_transfers: dict[int, list[int]] = {}
    completed_transfers: dict[int, list[int]] = {}
    command_responses: list[dict[str, Any]] = []
    command_results: list[dict[str, Any]] = []
    transport_timings: list[dict[str, Any]] = []

    try:
        for item in read_log(log_path):
            kind = item.get("kind")
            if kind == "header":
                metadata = item
                nodes = _node_map(metadata)
                continue
            if kind == "footer":
                footer = item
                continue
            if kind not in {"record", None}:
                marker_values = {
                    key: value
                    for key, value in item.items()
                    if key not in {"kind", "received_monotonic_ns"}
                }
                markers_writer.writerow(
                    {
                        "received_monotonic_ns": item.get(
                            "received_monotonic_ns", ""
                        ),
                        "kind": kind,
                        "values_json": json.dumps(
                            marker_values, separators=(",", ":")
                        ),
                    }
                )
            if kind == "host_transport_health":
                host_transport_writer.writerow(item)
                continue
            if kind == "command_queued":
                queued_commands.append(item)
                continue
            if kind == "transport_sent":
                sent_transfers.setdefault(int(item["transfer_id"]), []).append(
                    int(item["sent_monotonic_ns"])
                )
                continue
            if kind == "transport_write_complete":
                completed_transfers.setdefault(
                    int(item["transfer_id"]), []
                ).append(
                    int(item["completed_monotonic_ns"])
                )
                continue
            if kind == "command_response":
                command_responses.append(item)
                continue
            if kind != "record":
                continue

            fields = fields_by_name(item)
            common = {
                "received_monotonic_ns": item["received_monotonic_ns"],
                "source": item["source"],
                "destination": item.get("destination"),
                "transfer_id": item.get("transfer_id"),
                "record_type": item["record_type"],
                "record_type_value": item["record_type_value"],
                "flags": item["flags"],
                "record_id": item["record_id"],
                "session_id": item["session_id"],
                "timestamp_ticks": item["timestamp_ticks"],
                "clock_hz": item["clock_hz"],
                **_describe_address(item["source"], nodes, "source"),
                **_describe_address(item.get("destination"), nodes, "destination"),
                "fields_json": json.dumps(fields, separators=(",", ":")),
                **fields,
            }
            records_writer.writerow(common)

            record_type = item["record_type"]
            if record_type in {"sync_observation", "clock_pair"}:
                writers["sync_pairs"].writerow(common)
            if record_type == "sync_quality":
                writers["sync_quality"].writerow(common)
            if record_type == "transport_timing":
                writers["transport_timing"].writerow(common)
                transport_timings.append(common)
            if record_type.startswith("ttl_"):
                writers["ttl"].writerow(common)
            if record_type in {
                "digital_input",
                "output_change",
                "ttl_scheduled",
                "ttl_generated",
                "ttl_captured",
                "ttl_result",
                "light_gate",
                "command_result",
                "fault",
                "test_marker",
            }:
                writers["events"].writerow(common)
                if record_type == "command_result":
                    command_results.append(common)
            if record_type in {"health", "link_quality", "fault"}:
                writers["health"].writerow(common)
    finally:
        for handle in handles:
            handle.close()

    for grouped in (sent_transfers, completed_transfers):
        for timestamps in grouped.values():
            timestamps.sort()

    _write_command_outputs(
        output_directory,
        outputs,
        nodes,
        queued_commands,
        sent_transfers,
        completed_transfers,
        command_responses,
        command_results,
        transport_timings,
    )

    manifest = output_directory / "manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "source_log": str(log_path.resolve()),
                "header": metadata,
                "footer": footer,
                "outputs": {name: str(path.resolve()) for name, path in outputs.items()},
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    outputs["manifest"] = manifest
    return outputs


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Parse an Adelie Fairy log")
    parser.add_argument("log", type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        help="output directory, default is LOGNAME_parsed",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output = args.output or args.log.with_name(f"{args.log.stem}_parsed")
    paths = parse(args.log, output)
    for name, path in paths.items():
        print(f"{name}: {path}")


if __name__ == "__main__":
    main()
