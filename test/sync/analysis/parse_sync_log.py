from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

DEFAULT_NODE = "fairy"
NODE_ALIASES = {
    "reward_port": "fairy",
    "fairy": "fairy",
    "galapagos": "galapagos",
    "korora": "korora",
    "adelie": "adelie",
}
KNOWN_NODES = set(NODE_ALIASES)
MODEL_DIAGNOSTIC_TYPES = {
    "MODEL_STEP_REJECT",
    "MODEL_RESET",
    "FIT_REJECT",
    "ADMISSION_REJECT",
    "DUPLICATE_SYNC_IN_WINDOW",
    "WINDOW_REJECT",
    "WINDOW_TIMEOUT",
}
RECORD_PATTERN = re.compile(r"([A-Z][A-Z0-9_]*),")
TRACK_STATES = {"TRACK", "LOCAL"}

PAIR_COLUMNS = [
    "record_type",
    "node",
    "pulse",
    "hub_ticks",
    "local_ticks",
    "prospective_count",
    "has_previous",
    "local_delta_ticks",
    "local_interval_error_ticks",
    "transport_age_ticks",
    "segment_id",
    "source_line",
    "schema_version",
]

SYNC_COLUMNS = [
    "record_type",
    "node",
    "pulse",
    "hub_ticks",
    "local_ticks",
    "status_flags",
    "record_flags",
    "pending_count",
    "state",
    "slope_ppb",
    "local_reference_ticks",
    "hub_reference_ticks",
    "rms_ns",
    "prefit_residual_ns",
    "model_step_ns",
    "transport_age_ticks",
    "segment_id",
    "source_line",
    "schema_version",
]

EVENT_COLUMNS = [
    "record_type",
    "node",
    "event_id",
    "kind",
    "local_ticks",
    "local_hz",
    "hub_ticks",
    "state",
    "transport_age_ticks",
    "reference_node",
    "reference_event_id",
    "reference_hub_ticks",
    "error_ns",
    "matched",
    "segment_id",
    "source_line",
    "raw_line",
    "schema_version",
    # Compatibility aliases used by the previous analysis scripts.
    "remote_sequence",
    "converted_hub_ticks",
    "matched_hub_sequence",
    "hub_capture_ticks",
]

EVENT_MATCH_COLUMNS = [
    "record_type",
    "node",
    "event_id",
    "reference_node",
    "reference_event_id",
    "converted_hub_ticks",
    "reference_hub_ticks",
    "error_ns",
    "source_line",
    "raw_line",
    "schema_version",
]

GENERATOR_COLUMNS = [
    "record_type",
    "sequence",
    "generated_us",
    "wait_ms",
    "source_line",
    "raw_line",
]

TTL_RECORD_TYPES = {
    "TTL_PULSE_SCHEDULED",
    "TTL_PULSE_GENERATED",
    "TTL_PULSE_ACQUIRED",
    "TTL_PULSE_RESULT",
    "TTL_PULSE_TIMEOUT",
}

TTL_COLUMNS = [
    "record_type",
    "node",
    "test_segment_id",
    "sequence",
    "target_hub_ticks",
    "target_local_ticks",
    "pulse_width_us",
    "generated_local_ticks",
    "generated_hub_ticks",
    "generation_error_ns",
    "acquired_hub_ticks",
    "wire_offset_ns",
    "total_error_ns",
    "timeout_hub_ticks",
    "generated_seen",
    "acquired_seen",
    "source_line",
    "raw_line",
    "schema_version",
]

DIAGNOSTIC_COLUMNS = [
    "record_type",
    "node",
    "pulse",
    "value",
    "limit",
    "reason",
    "segment_before",
    "segment_after",
    "source_line",
    "raw_line",
]

LINK_COLUMNS = [
    "record_type",
    "node",
    "transport",
    "role",
    "state",
    "peer",
    "interval_us",
    "reason",
    "source_line",
    "raw_line",
]

FAULT_COLUMNS = [
    "record_type",
    "node",
    "category",
    "code",
    "value",
    "source_line",
    "raw_line",
]

STATUS_COLUMNS = [
    "record_type",
    "node",
    "fields",
    "source_line",
    "raw_line",
]

UNKNOWN_COLUMNS = [
    "source_line",
    "record_type",
    "raw_line",
    "error",
]

ADELIE_CLOCK_COLUMNS = [
    "sequence",
    "t1_adelie_ns",
    "t2_korora_ticks",
    "t3_korora_ticks",
    "t4_adelie_ns",
    "network_rtt_ns",
    "full_exchange_us",
    "midpoint_adelie_ns",
    "midpoint_korora_ticks",
    "source_file",
]

ADELIE_COMMAND_COLUMNS = [
    "sequence",
    "model_generation",
    "model_points",
    "model_slope_error_ppm",
    "model_min_network_rtt_us",
    "model_median_network_rtt_us",
    "adelie_t1_ns",
    "adelie_t7_ns",
    "total_rtt_us",
    "ble_down_est_us",
    "ble_up_est_us",
    "korora_rx_ticks",
    "i2c_tx_start_ticks",
    "fairy_rx_hub_ticks",
    "fairy_exec_hub_ticks",
    "korora_ack_rx_ticks",
    "korora_done_tx_ticks",
    "korora_queue_to_i2c_us",
    "korora_post_ack_us",
    "fairy_model_valid",
    "i2c_down_us",
    "fairy_action_us",
    "i2c_up_and_poll_us",
    "korora_fairy_rtt_us",
    "korora_internal_us",
    "source_file",
]

ADELIE_COMMAND_ALIASES = {
    "reward_rx_hub_ticks": "fairy_rx_hub_ticks",
    "reward_exec_hub_ticks": "fairy_exec_hub_ticks",
    "reward_model_valid": "fairy_model_valid",
    "reward_action_us": "fairy_action_us",
    "korora_reward_rtt_us": "korora_fairy_rtt_us",
}


def normalize_node(value: str) -> str:
    return NODE_ALIASES.get(value.strip(), value.strip())


def parse_int(value: str) -> int:
    return int(value.strip(), 0)


def is_int(value: str) -> bool:
    try:
        parse_int(value)
        return True
    except (ValueError, TypeError):
        return False


def extract_record(line: str) -> str | None:
    match = RECORD_PATTERN.search(line)
    if match is None:
        return None
    return line[match.start() :].strip()


def split_csv_record(record: str) -> list[str]:
    return next(csv.reader([record]))


def detect_node_and_base(
    fields: list[str],
    seen_nodes: set[str],
) -> tuple[str, int]:
    """Return normalized node name and index after the optional node field."""

    if len(fields) < 2:
        return DEFAULT_NODE, 1

    second = fields[1].strip()

    if is_int(second):
        return DEFAULT_NODE, 1

    if second in KNOWN_NODES or second in seen_nodes:
        seen_nodes.add(second)
        return normalize_node(second), 2

    # PAIR_RAW/SYNC and most node-qualified records have a numeric field after
    # the node.  This also accepts future node names without hard-coding them.
    if len(fields) >= 3 and is_int(fields[2]):
        seen_nodes.add(second)
        return normalize_node(second), 2

    return DEFAULT_NODE, 1


def write_csv(
    path: Path,
    columns: list[str],
    rows: list[dict[str, Any]],
) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def parse_pair(
    fields: list[str],
    node: str,
    base: int,
    segment_id: int,
    source_line: int,
    schema_version: int,
) -> dict[str, Any]:
    if len(fields) < base + 8:
        raise ValueError(f"PAIR_RAW requires 8 data fields, got {len(fields) - base}")

    return {
        "record_type": "PAIR_RAW",
        "node": node,
        "pulse": parse_int(fields[base]),
        "hub_ticks": parse_int(fields[base + 1]),
        "local_ticks": parse_int(fields[base + 2]),
        "prospective_count": parse_int(fields[base + 3]),
        "has_previous": parse_int(fields[base + 4]),
        "local_delta_ticks": parse_int(fields[base + 5]),
        "local_interval_error_ticks": parse_int(fields[base + 6]),
        "transport_age_ticks": parse_int(fields[base + 7]),
        "segment_id": segment_id,
        "source_line": source_line,
        "schema_version": schema_version,
    }


def parse_sync(
    fields: list[str],
    node: str,
    base: int,
    segment_id: int,
    source_line: int,
    schema_version: int,
) -> dict[str, Any]:
    if len(fields) < base + 14:
        raise ValueError(f"SYNC requires 14 data fields, got {len(fields) - base}")

    return {
        "record_type": "SYNC",
        "node": node,
        "pulse": parse_int(fields[base]),
        "hub_ticks": parse_int(fields[base + 1]),
        "local_ticks": parse_int(fields[base + 2]),
        "status_flags": parse_int(fields[base + 3]),
        "record_flags": parse_int(fields[base + 4]),
        "pending_count": parse_int(fields[base + 5]),
        "state": fields[base + 6].strip(),
        "slope_ppb": parse_int(fields[base + 7]),
        "local_reference_ticks": parse_int(fields[base + 8]),
        "hub_reference_ticks": parse_int(fields[base + 9]),
        "rms_ns": parse_int(fields[base + 10]),
        "prefit_residual_ns": parse_int(fields[base + 11]),
        "model_step_ns": parse_int(fields[base + 12]),
        "transport_age_ticks": parse_int(fields[base + 13]),
        "segment_id": segment_id,
        "source_line": source_line,
        "schema_version": schema_version,
    }


def blank_event(
    *,
    node: str,
    segment_id: int,
    source_line: int,
    raw_line: str,
    schema_version: int,
) -> dict[str, Any]:
    return {
        "record_type": "EVENT",
        "node": node,
        "event_id": "",
        "kind": "GPIO_RISE",
        "local_ticks": "",
        "local_hz": 16_000_000,
        "hub_ticks": "",
        "state": "",
        "transport_age_ticks": "",
        "reference_node": "",
        "reference_event_id": "",
        "reference_hub_ticks": "",
        "error_ns": "",
        "matched": 0,
        "segment_id": segment_id,
        "source_line": source_line,
        "raw_line": raw_line,
        "schema_version": schema_version,
        "remote_sequence": "",
        "converted_hub_ticks": "",
        "matched_hub_sequence": "",
        "hub_capture_ticks": "",
    }


def apply_event_aliases(row: dict[str, Any]) -> None:
    row["remote_sequence"] = row.get("event_id", "")
    row["converted_hub_ticks"] = row.get("hub_ticks", "")
    row["matched_hub_sequence"] = row.get("reference_event_id", "")
    row["hub_capture_ticks"] = row.get("reference_hub_ticks", "")


def parse_event(
    fields: list[str],
    raw_line: str,
    node: str,
    base: int,
    segment_id: int,
    source_line: int,
    schema_version: int,
    synthetic_event_ids: dict[str, int],
) -> dict[str, Any]:
    args = fields[base:]
    row = blank_event(
        node=node,
        segment_id=segment_id,
        source_line=source_line,
        raw_line=raw_line,
        schema_version=schema_version,
    )

    # Schema v3:
    # EVENT,node,event_id,kind,local_ticks,local_hz,hub_ticks,state,age
    if len(args) == 7 and not is_int(args[1]):
        row.update(
            {
                "event_id": parse_int(args[0]),
                "kind": args[1].strip(),
                "local_ticks": parse_int(args[2]),
                "local_hz": parse_int(args[3]),
                "hub_ticks": parse_int(args[4]),
                "state": args[5].strip(),
                "transport_age_ticks": parse_int(args[6]),
            }
        )
        apply_event_aliases(row)
        return row

    # Legacy matched event:
    # EVENT,node,event_id,local,converted,reference_id,reference,error_ns,state,age
    if len(args) >= 8 and is_int(args[0]) and is_int(args[1]):
        row.update(
            {
                "event_id": parse_int(args[0]),
                "kind": "GPIO_RISE",
                "local_ticks": parse_int(args[1]),
                "local_hz": 16_000_000,
                "hub_ticks": parse_int(args[2]),
                "reference_node": "korora",
                "reference_event_id": parse_int(args[3]),
                "reference_hub_ticks": parse_int(args[4]),
                "error_ns": parse_int(args[5]),
                "state": args[6].strip(),
                "transport_age_ticks": parse_int(args[7]),
                "matched": 1,
            }
        )
        apply_event_aliases(row)
        return row

    # Older short event:
    # EVENT,node,local,converted,state,status_flags,record_flags,pending,age
    if len(args) == 7 and is_int(args[0]) and is_int(args[1]):
        synthetic_event_ids[node] += 1
        row.update(
            {
                "event_id": synthetic_event_ids[node],
                "kind": "GPIO_RISE",
                "local_ticks": parse_int(args[0]),
                "local_hz": 16_000_000,
                "hub_ticks": parse_int(args[1]),
                "state": args[2].strip(),
                "transport_age_ticks": parse_int(args[6]),
            }
        )
        apply_event_aliases(row)
        return row

    raise ValueError(f"unsupported EVENT layout with {len(args)} data fields")


def parse_event_match(
    fields: list[str],
    raw_line: str,
    source_line: int,
    schema_version: int,
) -> dict[str, Any]:
    if len(fields) != 8:
        raise ValueError(f"EVENT_MATCH requires 7 data fields, got {len(fields) - 1}")

    return {
        "record_type": "EVENT_MATCH",
        "node": normalize_node(fields[1]),
        "event_id": parse_int(fields[2]),
        "reference_node": normalize_node(fields[3]),
        "reference_event_id": parse_int(fields[4]),
        "converted_hub_ticks": parse_int(fields[5]),
        "reference_hub_ticks": parse_int(fields[6]),
        "error_ns": parse_int(fields[7]),
        "source_line": source_line,
        "raw_line": raw_line,
        "schema_version": schema_version,
    }


def parse_generator(
    fields: list[str],
    raw_line: str,
    source_line: int,
) -> dict[str, Any]:
    if len(fields) < 4:
        raise ValueError(f"EVENT_GEN requires 3 data fields, got {len(fields) - 1}")

    return {
        "record_type": "EVENT_GEN",
        "sequence": parse_int(fields[1]),
        "generated_us": parse_int(fields[2]),
        "wait_ms": parse_int(fields[3]),
        "source_line": source_line,
        "raw_line": raw_line,
    }


def parse_ttl_record(
    fields: list[str],
    raw_line: str,
    source_line: int,
    schema_version: int,
    test_segment_id: int,
) -> dict[str, Any]:
    record_type = fields[0]
    row: dict[str, Any] = {
        "record_type": record_type,
        "node": "korora",
        "test_segment_id": test_segment_id,
        "sequence": "",
        "target_hub_ticks": "",
        "target_local_ticks": "",
        "pulse_width_us": "",
        "generated_local_ticks": "",
        "generated_hub_ticks": "",
        "generation_error_ns": "",
        "acquired_hub_ticks": "",
        "wire_offset_ns": "",
        "total_error_ns": "",
        "timeout_hub_ticks": "",
        "generated_seen": "",
        "acquired_seen": "",
        "source_line": source_line,
        "raw_line": raw_line,
        "schema_version": schema_version,
    }

    if record_type == "TTL_PULSE_SCHEDULED":
        if len(fields) != 6:
            raise ValueError(
                f"TTL_PULSE_SCHEDULED requires 5 data fields, got {len(fields) - 1}"
            )
        row.update(
            {
                "node": normalize_node(fields[1]),
                "sequence": parse_int(fields[2]),
                "target_hub_ticks": parse_int(fields[3]),
                "target_local_ticks": parse_int(fields[4]),
                "pulse_width_us": parse_int(fields[5]),
            }
        )
    elif record_type == "TTL_PULSE_GENERATED":
        if len(fields) != 7:
            raise ValueError(
                f"TTL_PULSE_GENERATED requires 6 data fields, got {len(fields) - 1}"
            )
        row.update(
            {
                "node": normalize_node(fields[1]),
                "sequence": parse_int(fields[2]),
                "generated_local_ticks": parse_int(fields[3]),
                "generated_hub_ticks": parse_int(fields[4]),
                "target_hub_ticks": parse_int(fields[5]),
                "generation_error_ns": parse_int(fields[6]),
            }
        )
    elif record_type == "TTL_PULSE_ACQUIRED":
        if len(fields) != 6:
            raise ValueError(
                f"TTL_PULSE_ACQUIRED requires 5 data fields, got {len(fields) - 1}"
            )
        row.update(
            {
                "node": normalize_node(fields[1]),
                "sequence": parse_int(fields[2]),
                "acquired_hub_ticks": parse_int(fields[3]),
                "target_hub_ticks": parse_int(fields[4]),
                "total_error_ns": parse_int(fields[5]),
            }
        )
    elif record_type == "TTL_PULSE_RESULT":
        if len(fields) != 9:
            raise ValueError(
                f"TTL_PULSE_RESULT requires 8 data fields, got {len(fields) - 1}"
            )
        row.update(
            {
                "sequence": parse_int(fields[1]),
                "target_hub_ticks": parse_int(fields[2]),
                "target_local_ticks": parse_int(fields[3]),
                "generated_hub_ticks": parse_int(fields[4]),
                "acquired_hub_ticks": parse_int(fields[5]),
                "generation_error_ns": parse_int(fields[6]),
                "wire_offset_ns": parse_int(fields[7]),
                "total_error_ns": parse_int(fields[8]),
            }
        )
    elif record_type == "TTL_PULSE_TIMEOUT":
        if len(fields) != 6:
            raise ValueError(
                f"TTL_PULSE_TIMEOUT requires 5 data fields, got {len(fields) - 1}"
            )
        row.update(
            {
                "sequence": parse_int(fields[1]),
                "target_hub_ticks": parse_int(fields[2]),
                "timeout_hub_ticks": parse_int(fields[3]),
                "generated_seen": parse_int(fields[4]),
                "acquired_seen": parse_int(fields[5]),
            }
        )
    else:
        raise ValueError(f"unsupported TTL record type {record_type}")

    return row


def parse_diagnostic(
    fields: list[str],
    raw_line: str,
    node: str,
    base: int,
    segment_before: int,
    segment_after: int,
    source_line: int,
    fallback_pulse: int | None,
) -> dict[str, Any]:
    record_type = fields[0]
    args = fields[base:]

    pulse: int | str = ""
    value: int | str = ""
    limit: int | str = ""
    reason = ""

    if record_type == "MODEL_STEP_REJECT":
        if len(args) >= 1 and is_int(args[0]):
            pulse = parse_int(args[0])
        if len(args) >= 2 and is_int(args[1]):
            value = parse_int(args[1])
        if len(args) >= 3 and is_int(args[2]):
            limit = parse_int(args[2])
        reason = ",".join(args[3:])
    elif record_type == "MODEL_RESET":
        if args and is_int(args[0]):
            pulse = parse_int(args[0])
            reason = ",".join(args[1:])
        else:
            pulse = fallback_pulse if fallback_pulse is not None else ""
            reason = ",".join(args)
    else:
        if args and is_int(args[0]):
            pulse = parse_int(args[0])
            reason = ",".join(args[1:])
        else:
            pulse = fallback_pulse if fallback_pulse is not None else ""
            reason = ",".join(args)

    return {
        "record_type": record_type,
        "node": node,
        "pulse": pulse,
        "value": value,
        "limit": limit,
        "reason": reason,
        "segment_before": segment_before,
        "segment_after": segment_after,
        "source_line": source_line,
        "raw_line": raw_line,
    }


def parse_link(
    fields: list[str],
    raw_line: str,
    source_line: int,
) -> dict[str, Any]:
    if len(fields) != 8:
        raise ValueError(f"LINK requires 7 data fields, got {len(fields) - 1}")
    return {
        "record_type": "LINK",
        "node": normalize_node(fields[1]),
        "transport": fields[2],
        "role": fields[3],
        "state": fields[4],
        "peer": fields[5],
        "interval_us": parse_int(fields[6]),
        "reason": fields[7],
        "source_line": source_line,
        "raw_line": raw_line,
    }


def parse_fault(
    fields: list[str],
    raw_line: str,
    source_line: int,
) -> dict[str, Any]:
    if len(fields) != 5:
        raise ValueError(f"FAULT requires 4 data fields, got {len(fields) - 1}")
    return {
        "record_type": "FAULT",
        "node": normalize_node(fields[1]),
        "category": fields[2],
        "code": parse_int(fields[3]),
        "value": parse_int(fields[4]),
        "source_line": source_line,
        "raw_line": raw_line,
    }


def parse_status(
    fields: list[str],
    raw_line: str,
    node: str,
    base: int,
    source_line: int,
) -> dict[str, Any]:
    return {
        "record_type": fields[0],
        "node": node,
        "fields": ",".join(fields[base:]),
        "source_line": source_line,
        "raw_line": raw_line,
    }


def merge_event_matches(
    events: list[dict[str, Any]],
    matches: list[dict[str, Any]],
) -> None:
    # Multiple Adelie command stages can share (node,event_id). EVENT_MATCH is
    # only meaningful for GPIO_RISE, so prefer that row when resolving a key.
    by_key: dict[tuple[str, int], list[dict[str, Any]]] = defaultdict(list)

    for event in events:
        event_id = event.get("event_id")
        if event_id == "" or event_id is None:
            continue
        by_key[(str(event["node"]), int(event_id))].append(event)

    for match in matches:
        key = (str(match["node"]), int(match["event_id"]))
        candidates = by_key.get(key, [])
        if not candidates:
            continue

        event = next(
            (candidate for candidate in candidates if candidate["kind"] == "GPIO_RISE"),
            candidates[0],
        )
        event["hub_ticks"] = match["converted_hub_ticks"]
        event["reference_node"] = match["reference_node"]
        event["reference_event_id"] = match["reference_event_id"]
        event["reference_hub_ticks"] = match["reference_hub_ticks"]
        event["error_ns"] = match["error_ns"]
        event["matched"] = 1
        apply_event_aliases(event)


def read_csv_rows(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open("r", newline="", encoding="utf-8", errors="replace") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            return [], []
        return [field.strip() for field in reader.fieldnames], [
            dict(row) for row in reader
        ]


def classify_adelie_csv(path: Path) -> str | None:
    fields, _ = read_csv_rows(path)
    names = set(fields)

    if {"t1_adelie_ns", "t2_korora_ticks", "midpoint_korora_ticks"}.issubset(names):
        return "clock"
    if {"total_rtt_us", "korora_rx_ticks", "korora_done_tx_ticks"}.issubset(names):
        return "commands"
    return None


def resolve_adelie_paths(paths: Iterable[Path]) -> tuple[Path | None, Path | None]:
    supplied = list(paths)
    if not supplied:
        return None, None
    if len(supplied) > 2:
        raise ValueError("--adelie accepts one directory/file or two CSV files")

    candidates: list[Path] = []

    for path in supplied:
        if not path.exists():
            raise FileNotFoundError(path)
        if path.is_dir():
            exact = [
                path / "adelie_clock_samples.csv",
                path / "adelie_latency.csv",
            ]
            candidates.extend(candidate for candidate in exact if candidate.exists())
            if not candidates:
                candidates.extend(sorted(path.glob("*.csv")))
        else:
            candidates.append(path)
            # If only one default-named file was supplied, discover its sibling.
            if len(supplied) == 1:
                siblings = [
                    path.parent / "adelie_clock_samples.csv",
                    path.parent / "adelie_latency.csv",
                ]
                candidates.extend(
                    sibling
                    for sibling in siblings
                    if sibling.exists() and sibling != path
                )

    # Preserve order while removing duplicates.
    unique: list[Path] = []
    seen: set[Path] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved not in seen:
            unique.append(candidate)
            seen.add(resolved)

    clock: Path | None = None
    commands: Path | None = None

    for candidate in unique:
        kind = classify_adelie_csv(candidate)
        if kind == "clock" and clock is None:
            clock = candidate
        elif kind == "commands" and commands is None:
            commands = candidate

    if clock is None and commands is None:
        raise ValueError(
            "could not identify an Adelie clock or latency CSV from --adelie"
        )

    return clock, commands


def normalize_adelie_clock(path: Path | None) -> list[dict[str, Any]]:
    if path is None:
        return []

    _, rows = read_csv_rows(path)
    normalized: list[dict[str, Any]] = []

    for row in rows:
        output = {column: row.get(column, "") for column in ADELIE_CLOCK_COLUMNS}
        output["source_file"] = str(path.resolve())
        normalized.append(output)

    return normalized


def normalize_adelie_commands(path: Path | None) -> list[dict[str, Any]]:
    if path is None:
        return []

    _, rows = read_csv_rows(path)
    normalized: list[dict[str, Any]] = []

    for source in rows:
        aliased = dict(source)
        for old, new in ADELIE_COMMAND_ALIASES.items():
            if (new not in aliased or aliased.get(new, "") == "") and old in aliased:
                aliased[new] = aliased[old]

        output = {column: aliased.get(column, "") for column in ADELIE_COMMAND_COLUMNS}
        output["source_file"] = str(path.resolve())
        normalized.append(output)

    return normalized


def parse_korora_log(path: Path) -> dict[str, list[dict[str, Any]] | int]:
    pairs: list[dict[str, Any]] = []
    sync_rows: list[dict[str, Any]] = []
    events: list[dict[str, Any]] = []
    event_matches: list[dict[str, Any]] = []
    generators: list[dict[str, Any]] = []
    ttl_records: list[dict[str, Any]] = []
    diagnostics: list[dict[str, Any]] = []
    links: list[dict[str, Any]] = []
    faults: list[dict[str, Any]] = []
    status: list[dict[str, Any]] = []
    unknown: list[dict[str, Any]] = []

    segment_ids: dict[str, int] = defaultdict(int)
    last_pulses: dict[str, int] = {}
    synthetic_event_ids: dict[str, int] = defaultdict(int)
    seen_nodes = set(KNOWN_NODES)
    schema_version = 1
    ttl_test_segment_id = 0
    last_ttl_scheduled_sequence: int | None = None

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for source_line, original_line in enumerate(handle, start=1):
            raw = extract_record(original_line)
            if raw is None:
                continue

            fields: list[str] = []

            try:
                fields = [value.strip() for value in split_csv_record(raw)]
                if not fields:
                    continue

                record_type = fields[0]

                if record_type == "SCHEMA":
                    if len(fields) != 2:
                        raise ValueError("SCHEMA requires one version field")
                    schema_version = parse_int(fields[1])
                    status.append(
                        {
                            "record_type": "SCHEMA",
                            "node": "korora",
                            "fields": str(schema_version),
                            "source_line": source_line,
                            "raw_line": raw,
                        }
                    )
                    continue

                if record_type == "EVENT_GEN":
                    generators.append(parse_generator(fields, raw, source_line))
                    continue

                if record_type == "EVENT_MATCH":
                    event_matches.append(
                        parse_event_match(fields, raw, source_line, schema_version)
                    )
                    continue

                if record_type in TTL_RECORD_TYPES:
                    if record_type == "TTL_PULSE_SCHEDULED":
                        scheduled_sequence = parse_int(fields[2])
                        if (
                            last_ttl_scheduled_sequence is not None
                            and scheduled_sequence <= last_ttl_scheduled_sequence
                        ):
                            ttl_test_segment_id += 1
                        last_ttl_scheduled_sequence = scheduled_sequence

                    ttl_records.append(
                        parse_ttl_record(
                            fields,
                            raw,
                            source_line,
                            schema_version,
                            ttl_test_segment_id,
                        )
                    )
                    continue

                if record_type == "LINK":
                    links.append(parse_link(fields, raw, source_line))
                    continue

                if record_type == "FAULT":
                    faults.append(parse_fault(fields, raw, source_line))
                    continue

                node, base = detect_node_and_base(fields, seen_nodes)

                if record_type == "PAIR_RAW":
                    row = parse_pair(
                        fields,
                        node,
                        base,
                        segment_ids[node],
                        source_line,
                        schema_version,
                    )
                    pairs.append(row)
                    last_pulses[node] = int(row["pulse"])
                elif record_type == "SYNC":
                    row = parse_sync(
                        fields,
                        node,
                        base,
                        segment_ids[node],
                        source_line,
                        schema_version,
                    )
                    sync_rows.append(row)
                    last_pulses[node] = int(row["pulse"])
                elif record_type == "EVENT":
                    events.append(
                        parse_event(
                            fields,
                            raw,
                            node,
                            base,
                            segment_ids[node],
                            source_line,
                            schema_version,
                            synthetic_event_ids,
                        )
                    )
                elif record_type in MODEL_DIAGNOSTIC_TYPES:
                    segment_before = segment_ids[node]
                    segment_after = segment_before
                    if record_type == "MODEL_RESET":
                        segment_ids[node] += 1
                        segment_after = segment_ids[node]

                    diagnostics.append(
                        parse_diagnostic(
                            fields,
                            raw,
                            node,
                            base,
                            segment_before,
                            segment_after,
                            source_line,
                            last_pulses.get(node),
                        )
                    )
                else:
                    status.append(parse_status(fields, raw, node, base, source_line))

            except (ValueError, IndexError, csv.Error) as error:
                unknown.append(
                    {
                        "source_line": source_line,
                        "record_type": fields[0] if fields else "",
                        "raw_line": raw,
                        "error": str(error),
                    }
                )

    merge_event_matches(events, event_matches)

    return {
        "schema_version": schema_version,
        "pairs": pairs,
        "sync": sync_rows,
        "events": events,
        "event_matches": event_matches,
        "generators": generators,
        "ttl_records": ttl_records,
        "diagnostics": diagnostics,
        "links": links,
        "faults": faults,
        "status": status,
        "unknown": unknown,
    }


def parse_inputs(
    *,
    korora: Path | None,
    adelie: list[Path] | None,
    output: Path,
) -> dict[str, Any]:
    if korora is None and not adelie:
        raise ValueError("provide at least one of --korora or --adelie")

    output.mkdir(parents=True, exist_ok=True)

    if korora is not None:
        if not korora.exists():
            raise FileNotFoundError(korora)
        parsed = parse_korora_log(korora)
    else:
        parsed = {
            "schema_version": 0,
            "pairs": [],
            "sync": [],
            "events": [],
            "event_matches": [],
            "generators": [],
            "ttl_records": [],
            "diagnostics": [],
            "links": [],
            "faults": [],
            "status": [],
            "unknown": [],
        }

    clock_path: Path | None = None
    command_path: Path | None = None
    if adelie:
        clock_path, command_path = resolve_adelie_paths(adelie)

    adelie_clock = normalize_adelie_clock(clock_path)
    adelie_commands = normalize_adelie_commands(command_path)

    write_csv(output / "pairs.csv", PAIR_COLUMNS, parsed["pairs"])
    write_csv(output / "sync.csv", SYNC_COLUMNS, parsed["sync"])
    write_csv(output / "events.csv", EVENT_COLUMNS, parsed["events"])
    # Compatibility with the previous script name.
    write_csv(output / "external_events.csv", EVENT_COLUMNS, parsed["events"])
    write_csv(
        output / "event_matches.csv",
        EVENT_MATCH_COLUMNS,
        parsed["event_matches"],
    )
    write_csv(
        output / "generator_events.csv",
        GENERATOR_COLUMNS,
        parsed["generators"],
    )
    write_csv(
        output / "ttl_records.csv",
        TTL_COLUMNS,
        parsed["ttl_records"],
    )
    write_csv(
        output / "diagnostics.csv",
        DIAGNOSTIC_COLUMNS,
        parsed["diagnostics"],
    )
    write_csv(output / "links.csv", LINK_COLUMNS, parsed["links"])
    write_csv(output / "faults.csv", FAULT_COLUMNS, parsed["faults"])
    write_csv(output / "status.csv", STATUS_COLUMNS, parsed["status"])
    write_csv(output / "unknown.csv", UNKNOWN_COLUMNS, parsed["unknown"])
    write_csv(output / "adelie_clock.csv", ADELIE_CLOCK_COLUMNS, adelie_clock)
    write_csv(
        output / "adelie_commands.csv",
        ADELIE_COMMAND_COLUMNS,
        adelie_commands,
    )

    manifest = {
        "korora_input": str(korora.resolve()) if korora is not None else None,
        "korora_schema_version": parsed["schema_version"],
        "adelie_clock_input": (
            str(clock_path.resolve()) if clock_path is not None else None
        ),
        "adelie_commands_input": (
            str(command_path.resolve()) if command_path is not None else None
        ),
        "counts": {
            "pairs": len(parsed["pairs"]),
            "sync": len(parsed["sync"]),
            "events": len(parsed["events"]),
            "event_matches": len(parsed["event_matches"]),
            "ttl_records": len(parsed["ttl_records"]),
            "diagnostics": len(parsed["diagnostics"]),
            "links": len(parsed["links"]),
            "faults": len(parsed["faults"]),
            "unknown": len(parsed["unknown"]),
            "adelie_clock": len(adelie_clock),
            "adelie_commands": len(adelie_commands),
        },
    }

    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )

    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=("Parse a Korora serial log and Adelie clock/latency CSV files.")
    )
    parser.add_argument(
        "legacy_input",
        nargs="?",
        type=Path,
        help="legacy positional alias for --korora",
    )
    parser.add_argument(
        "--korora",
        type=Path,
        help="Korora raw serial log",
    )
    parser.add_argument(
        "--adelie",
        nargs="+",
        type=Path,
        help=(
            "Adelie output directory, one Adelie CSV, or both "
            "adelie_clock_samples.csv and adelie_latency.csv"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("parsed"),
        help="output directory",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    korora = args.korora or args.legacy_input

    try:
        manifest = parse_inputs(
            korora=korora,
            adelie=args.adelie,
            output=args.output,
        )
    except (ValueError, FileNotFoundError) as error:
        raise SystemExit(str(error)) from error

    counts = manifest["counts"]
    print(f"Korora PAIR_RAW records: {counts['pairs']}")
    print(f"Korora SYNC records:     {counts['sync']}")
    print(f"Korora EVENT records:    {counts['events']}")
    print(f"Korora EVENT_MATCH rows: {counts['event_matches']}")
    print(f"Korora TTL records:      {counts['ttl_records']}")
    print(f"Adelie clock samples:    {counts['adelie_clock']}")
    print(f"Adelie command rows:     {counts['adelie_commands']}")
    print(f"Parse failures:          {counts['unknown']}")
    print(f"Results written to:      {args.output}")


if __name__ == "__main__":
    main()
