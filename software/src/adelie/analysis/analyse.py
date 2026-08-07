from __future__ import annotations

import argparse
from collections import Counter
import csv
import json
import math
from pathlib import Path
import statistics
from typing import Any

import numpy as np


def _read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _number(row: dict[str, str], key: str) -> float | None:
    value = row.get(key, "")
    if value in ("", None):
        return None
    try:
        return float(value)
    except ValueError:
        return None


def _summary(values: list[float]) -> dict[str, float | int]:
    finite = [value for value in values if math.isfinite(value)]
    if not finite:
        return {"count": 0}
    ordered = sorted(finite)
    return {
        "count": len(ordered),
        "mean": statistics.fmean(ordered),
        "standard_deviation": statistics.stdev(ordered) if len(ordered) > 1 else 0.0,
        "median": statistics.median(ordered),
        "p95": float(np.percentile(ordered, 95)),
        "p99": float(np.percentile(ordered, 99)),
        "minimum": ordered[0],
        "maximum": ordered[-1],
    }


def _integer(row: dict[str, str], key: str) -> int | None:
    value = _number(row, key)
    return int(value) if value is not None else None


def _node_name(row: dict[str, str]) -> str:
    if row.get("source_node"):
        return row["source_node"]
    source = _integer(row, "source")
    return f"node_0x{source:02x}" if source is not None else "unknown"


def _node_address(row: dict[str, str]) -> int | None:
    return _integer(row, "source_address") or _integer(row, "source")


def _values(rows: list[dict[str, str]], column: str) -> list[float]:
    return [value for row in rows if (value := _number(row, column)) is not None]


HEALTH_COUNTERS = (
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
)

HOST_COUNTERS = (
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
    "clock_exchange_count",
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
)


MIRRORED_HOST_COUNTERS = {
    "korora_ble_dropped": "dropped_records",
    "korora_rs485_errors": "transport_errors",
    "korora_rs485_retries": "retry_count",
    "korora_rs485_timeouts": "timeout_count",
    "korora_rs485_decode_errors": "transport_decode_errors",
    "korora_rs485_reassembly_errors": "transport_reassembly_errors",
    "korora_rs485_transmit_errors": "transport_transmit_errors",
    "korora_ttl_capture_edges": "ttl_capture_count",
    "korora_ttl_capture_drops": "ttl_capture_drops",
}


def _preferred_counter_rows(
    rows: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Remove host copies of counters already present in Korora health."""
    direct = {
        (str(row["source_node"]), str(row["metric"]))
        for row in rows
    }
    output: list[dict[str, Any]] = []
    for row in rows:
        node = str(row["source_node"])
        metric = str(row["metric"])
        direct_metric = MIRRORED_HOST_COUNTERS.get(
            metric) if node == "adelie_host" else None
        if direct_metric is not None and ("korora", direct_metric) in direct:
            continue
        if direct_metric is not None:
            row = {**row, "source_node": "korora", "metric": direct_metric}
        output.append(row)
    return output


def _timestamp_ns(row: dict[str, str]) -> int | None:
    value = _number(row, "received_monotonic_ns")
    if value is None:
        value = _number(row, "sample_ns")
    return int(value) if value is not None else None


def _write_rows(path: Path, columns: list[str], rows: list[dict[str, Any]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def _counter_delta_rows(
    health_rows: list[dict[str, str]],
    host_rows: list[dict[str, str]],
    origin_ns: int,
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    groups: list[tuple[str, list[dict[str, str]], tuple[str, ...]]] = []
    for node in sorted({_node_name(row) for row in health_rows}):
        groups.append(
            (
                node,
                [row for row in health_rows if _node_name(row) == node],
                HEALTH_COUNTERS,
            )
        )
    if host_rows:
        groups.append(("adelie_host", host_rows, HOST_COUNTERS))

    for node, rows, metrics in groups:
        ordered = sorted(
            (row for row in rows if _timestamp_ns(row) is not None),
            key=lambda row: _timestamp_ns(row) or 0,
        )
        for metric in metrics:
            previous_value: float | None = None
            previous_ns: int | None = None
            for row in ordered:
                value = _number(row, metric)
                timestamp_ns = _timestamp_ns(row)
                if value is None or timestamp_ns is None:
                    continue
                if previous_value is not None and previous_ns is not None:
                    interval_s = (timestamp_ns - previous_ns) / 1e9
                    reset = value < previous_value
                    delta = value if reset else value - previous_value
                    output.append(
                        {
                            "received_monotonic_ns": timestamp_ns,
                            "elapsed_s": (timestamp_ns - origin_ns) / 1e9,
                            "source_node": node,
                            "metric": metric,
                            "previous": previous_value,
                            "current": value,
                            "delta": delta,
                            "interval_s": interval_s,
                            "rate_per_s": (
                                delta / interval_s if interval_s > 0 else math.nan
                            ),
                            "counter_reset": reset,
                        }
                    )
                previous_value = value
                previous_ns = timestamp_ns
    return output


def _record_gap_rows(
    record_rows: list[dict[str, str]], origin_ns: int
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for node in sorted({_node_name(row) for row in record_rows}):
        timestamps = sorted(
            timestamp
            for row in record_rows
            if _node_name(row) == node
            if (timestamp := _timestamp_ns(row)) is not None
        )
        intervals = [
            (later - earlier) / 1e9
            for earlier, later in zip(timestamps, timestamps[1:])
            if later > earlier
        ]
        if not intervals:
            continue
        baseline = statistics.median(intervals)
        threshold = max(2.0, baseline * 8.0)
        for earlier, later, gap_s in zip(timestamps, timestamps[1:], intervals):
            output.append(
                {
                    "source_node": node,
                    "previous_monotonic_ns": earlier,
                    "received_monotonic_ns": later,
                    "start_elapsed_s": (earlier - origin_ns) / 1e9,
                    "end_elapsed_s": (later - origin_ns) / 1e9,
                    "gap_s": gap_s,
                    "median_interval_s": baseline,
                    "gap_factor": gap_s / baseline if baseline > 0 else math.nan,
                    "anomaly_threshold_s": threshold,
                    "is_anomaly": gap_s >= threshold,
                }
            )
    return output


def _phase_columns(start_s: float) -> dict[str, float]:
    # TIMER free running 32 bit wrap, RTC2 overflow, and two RTC2 overflows.
    return {
        "phase_268_435456_s": start_s % 268.435456,
        "phase_512_s": start_s % 512.0,
        "phase_1024_s": start_s % 1024.0,
    }


def _circular_phase_span(values: list[float], period: float) -> float:
    if len(values) < 2:
        return math.nan
    phases = sorted(value % period for value in values)
    gaps = [later - earlier for earlier, later in zip(phases, phases[1:])]
    gaps.append(period - phases[-1] + phases[0])
    return period - max(gaps)


def _anomaly_rows(
    deltas: list[dict[str, Any]],
    gaps: list[dict[str, Any]],
    command_rows: list[dict[str, str]],
    host_rows: list[dict[str, str]],
    origin_ns: int,
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []

    preferred_deltas = _preferred_counter_rows(deltas)
    positive_groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    all_groups: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for row in preferred_deltas:
        key = (str(row["source_node"]), str(row["metric"]))
        all_groups.setdefault(key, []).append(row)
        if bool(row["counter_reset"]):
            start = float(row["elapsed_s"])
            output.append(
                {
                    "category": "counter_reset",
                    "source_node": row["source_node"],
                    "metric": row["metric"],
                    "start_s": start,
                    "end_s": start,
                    "duration_s": 0.0,
                    "delta": row["current"],
                    "peak_rate_per_s": math.nan,
                    "detail": "counter decreased and restarted",
                    **_phase_columns(start),
                }
            )
        elif float(row["delta"]) > 0:
            positive_groups.setdefault(key, []).append(row)

    for (node, metric), rows in positive_groups.items():
        all_rates = [
            max(0.0, float(row["rate_per_s"]))
            for row in all_groups[(node, metric)]
            if math.isfinite(float(row["rate_per_s"]))
        ]
        baseline = statistics.median(all_rates) if all_rates else 0.0
        mad = (
            statistics.median(abs(value - baseline) for value in all_rates)
            if all_rates
            else 0.0
        )
        loss_counter = metric in {"dropped_records", "ttl_capture_drops"}
        host_transport_counter = node == "adelie_host" and metric in {
            "rx_decode_errors",
            "rx_reassembly_errors",
            "rx_address_errors",
            "tx_write_errors",
        }
        rate_threshold = max(5.0, baseline * 3.0, baseline + 8.0 * mad)
        if not loss_counter and not host_transport_counter:
            rows = [
                row for row in rows if float(row["rate_per_s"]) >= rate_threshold
            ]
        if not rows:
            continue
        rows.sort(key=lambda row: float(row["elapsed_s"]))
        groups: list[list[dict[str, Any]]] = []
        merge_gap_s = 30.0 if loss_counter else 10.0
        for row in rows:
            if not groups or (
                float(row["elapsed_s"]) - float(groups[-1][-1]["elapsed_s"])
                > merge_gap_s
            ):
                groups.append([row])
            else:
                groups[-1].append(row)
        for group in groups:
            start = max(
                0.0,
                float(group[0]["elapsed_s"])
                - max(0.0, float(group[0]["interval_s"])),
            )
            end = float(group[-1]["elapsed_s"])
            output.append(
                {
                    "category": (
                        "record_loss_burst"
                        if loss_counter
                        else "transport_error_burst"
                    ),
                    "source_node": node,
                    "metric": metric,
                    "start_s": start,
                    "end_s": end,
                    "duration_s": max(
                        float(group[-1]["interval_s"]), end - start
                    ),
                    "delta": sum(float(row["delta"]) for row in group),
                    "peak_rate_per_s": max(
                        float(row["rate_per_s"]) for row in group
                    ),
                    "detail": (
                        f"{len(group)} positive samples merged across gaps up to "
                        f"{merge_gap_s:.0f} s; baseline {baseline:.3f}/s"
                    ),
                    **_phase_columns(start),
                }
            )

    for row in gaps:
        if not bool(row["is_anomaly"]):
            continue
        start = float(row["start_elapsed_s"])
        output.append(
            {
                "category": "record_silence",
                "source_node": row["source_node"],
                "metric": "record_interarrival",
                "start_s": start,
                "end_s": float(row["end_elapsed_s"]),
                "duration_s": float(row["gap_s"]),
                "delta": math.nan,
                "peak_rate_per_s": math.nan,
                "detail": f"{float(row['gap_factor']):.1f} times median interval",
                **_phase_columns(start),
            }
        )

    valid_rtt = [
        row
        for row in command_rows
        if _number(row, "transport_rtt_us") is not None
        and _number(row, "sent_monotonic_ns") is not None
    ]
    rtts = [float(row["transport_rtt_us"]) for row in valid_rtt]
    if len(rtts) >= 8:
        median = statistics.median(rtts)
        mad = statistics.median(abs(value - median) for value in rtts)
        threshold = max(250_000.0, median + 12.0 * mad)
        for row in valid_rtt:
            value = float(row["transport_rtt_us"])
            if value < threshold:
                continue
            start = (int(row["sent_monotonic_ns"]) - origin_ns) / 1e9
            output.append(
                {
                    "category": "command_rtt_outlier",
                    "source_node": row.get("destination_node") or "unknown",
                    "metric": row.get("opcode") or "unknown",
                    "start_s": start,
                    "end_s": start + value / 1e6,
                    "duration_s": value / 1e6,
                    "delta": value,
                    "peak_rate_per_s": math.nan,
                    "detail": f"RTT {value / 1000:.3f} ms, threshold {threshold / 1000:.3f} ms",
                    **_phase_columns(start),
                }
            )

        ordered_rtt = sorted(
            valid_rtt, key=lambda row: int(float(row["sent_monotonic_ns"]))
        )
        window = 12
        changes: list[dict[str, Any]] = []
        for index in range(window, len(ordered_rtt) - window):
            before = statistics.median(
                float(row["transport_rtt_us"])
                for row in ordered_rtt[index - window: index]
            )
            after = statistics.median(
                float(row["transport_rtt_us"])
                for row in ordered_rtt[index: index + window]
            )
            difference = after - before
            if abs(difference) < 30_000.0:
                continue
            timestamp = int(float(ordered_rtt[index]["sent_monotonic_ns"]))
            changes.append(
                {
                    "timestamp": timestamp,
                    "start_s": (timestamp - origin_ns) / 1e9,
                    "before": before,
                    "after": after,
                    "difference": difference,
                }
            )
        consolidated: list[list[dict[str, Any]]] = []
        for change in changes:
            if not consolidated or (
                float(change["start_s"])
                - float(consolidated[-1][-1]["start_s"])
                > 30.0
            ):
                consolidated.append([change])
            else:
                consolidated[-1].append(change)
        for group in consolidated:
            strongest = max(group, key=lambda row: abs(
                float(row["difference"])))
            start = float(strongest["start_s"])
            output.append(
                {
                    "category": "command_rtt_regime_change",
                    "source_node": "korora",
                    "metric": "clock_exchange",
                    "start_s": start,
                    "end_s": start,
                    "duration_s": 0.0,
                    "delta": float(strongest["difference"]),
                    "peak_rate_per_s": math.nan,
                    "detail": (
                        f"rolling median changed from "
                        f"{float(strongest['before']) / 1000:.3f} to "
                        f"{float(strongest['after']) / 1000:.3f} ms"
                    ),
                    **_phase_columns(start),
                }
            )

    invalid_clock = [
        row
        for row in host_rows
        if str(row.get("clock_valid", "")).lower() in {"false", "0"}
        and _timestamp_ns(row) is not None
    ]
    if invalid_clock:
        ordered = sorted(
            invalid_clock, key=lambda row: _timestamp_ns(row) or 0)
        groups: list[list[dict[str, str]]] = []
        for row in ordered:
            if not groups or (
                (_timestamp_ns(row) or 0) -
                (_timestamp_ns(groups[-1][-1]) or 0)
                > 2_500_000_000
            ):
                groups.append([row])
            else:
                groups[-1].append(row)
        for group in groups:
            start = ((_timestamp_ns(group[0]) or origin_ns) - origin_ns) / 1e9
            end = ((_timestamp_ns(group[-1]) or origin_ns) - origin_ns) / 1e9
            output.append(
                {
                    "category": "host_clock_invalid",
                    "source_node": "adelie_host",
                    "metric": "clock_valid",
                    "start_s": start,
                    "end_s": end,
                    "duration_s": end - start,
                    "delta": len(group),
                    "peak_rate_per_s": math.nan,
                    "detail": f"{len(group)} invalid diagnostic samples",
                    **_phase_columns(start),
                }
            )

    output.sort(key=lambda row: (float(row["start_s"]), str(row["category"])))
    for index, row in enumerate(output, 1):
        row["anomaly_id"] = index
    return output


def _record_type_rows(record_rows: list[dict[str, str]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    keys = sorted(
        {(_node_name(row), row.get("record_type") or "unknown")
         for row in record_rows}
    )
    for node, record_type in keys:
        selected = [
            row
            for row in record_rows
            if _node_name(row) == node
            and (row.get("record_type") or "unknown") == record_type
        ]
        times = sorted(
            timestamp
            for row in selected
            if (timestamp := _timestamp_ns(row)) is not None
        )
        intervals = [
            (later - earlier) / 1e9
            for earlier, later in zip(times, times[1:])
            if later > earlier
        ]
        duration = (times[-1] - times[0]) / 1e9 if len(times) > 1 else 0.0
        interval_summary = _summary(intervals)
        output.append(
            {
                "source_node": node,
                "record_type": record_type,
                "count": len(selected),
                "duration_s": duration,
                "rate_hz": len(selected) / duration if duration > 0 else 0.0,
                "median_interval_s": interval_summary.get("median", math.nan),
                "p95_interval_s": interval_summary.get("p95", math.nan),
                "maximum_interval_s": interval_summary.get("maximum", math.nan),
            }
        )
    return output


def _build_timeseries(
    record_rows: list[dict[str, str]],
    quality_rows: list[dict[str, str]],
    health_rows: list[dict[str, str]],
    host_rows: list[dict[str, str]],
    command_rows: list[dict[str, str]],
    deltas: list[dict[str, Any]],
    origin_ns: int,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    samples: dict[int, dict[str, float]] = {}
    aggregates: dict[tuple[int, str], list[float]] = {}

    def second(timestamp_ns: int) -> int:
        return max(0, int((timestamp_ns - origin_ns) // 1_000_000_000))

    def add(timestamp_ns: int, feature: str, value: float) -> None:
        aggregates.setdefault(
            (second(timestamp_ns), feature), []).append(value)

    for row in record_rows:
        timestamp = _timestamp_ns(row)
        if timestamp is not None:
            add(timestamp, f"records_per_s__{_node_name(row)}", 1.0)
            add(
                timestamp,
                f"records_per_s__{_node_name(row)}__{row.get('record_type') or 'unknown'}",
                1.0,
            )
    for row in _preferred_counter_rows(deltas):
        timestamp = _integer(row, "received_monotonic_ns")
        if timestamp is not None:
            interval_s = max(0.001, float(row["interval_s"]))
            start_ns = timestamp - int(interval_s * 1e9)
            start_bucket = second(start_ns)
            end_bucket = second(timestamp)
            feature = f"counter_rate__{row['source_node']}__{row['metric']}"
            for bucket in range(start_bucket, end_bucket + 1):
                aggregates.setdefault((bucket, feature), []).append(
                    float(row["rate_per_s"])
                )
    for row in quality_rows:
        timestamp = _timestamp_ns(row)
        if timestamp is None:
            continue
        for column, suffix, scale in (
            ("rms_ns", "sync_rms_us", 1 / 1000),
            ("skew_ppb", "skew_ppm", 1 / 1000),
            ("model_points", "model_points", 1),
        ):
            value = _number(row, column)
            if value is not None:
                add(timestamp, f"{suffix}__{_node_name(row)}", value * scale)
    for row in health_rows:
        timestamp = _timestamp_ns(row)
        if timestamp is None:
            continue
        for column in ("queue_depth", "rssi_dbm"):
            value = _number(row, column)
            if value is not None:
                add(timestamp, f"{column}__{_node_name(row)}", value)
    for row in host_rows:
        timestamp = _timestamp_ns(row)
        if timestamp is None:
            continue
        for column in (
            "outgoing_queue",
            "last_rx_age_ms",
            "last_tx_age_ms",
            "clock_rms_us",
            "clock_median_rtt_us",
            "clock_skew_ppm",
            "clock_last_rtt_us",
        ):
            value = _number(row, column)
            if value is not None:
                add(timestamp, f"host__{column}", value)
    for row in command_rows:
        timestamp = _integer(row, "sent_monotonic_ns")
        value = _number(row, "transport_rtt_us")
        if timestamp is not None and value is not None:
            add(timestamp, "command_rtt_us", value)

    count_features = {
        feature
        for _, feature in aggregates
        if feature.startswith("records_per_s__")
        or feature.startswith("counter_rate__")
    }
    for (bucket, feature), values in aggregates.items():
        samples.setdefault(bucket, {})[feature] = (
            (
                sum(values)
                if feature.startswith("records_per_s__")
                else statistics.fmean(values)
            )
            if feature in count_features
            else statistics.fmean(values)
        )
    features = sorted({feature for values in samples.values()
                      for feature in values})
    rows = [
        {
            "elapsed_s": bucket,
            **{
                feature: values.get(
                    feature, 0.0 if feature in count_features else "")
                for feature in features
            },
        }
        for bucket, values in sorted(samples.items())
    ]

    correlations: list[dict[str, Any]] = []
    for first_index, first in enumerate(features):
        for second_name in features[first_index + 1:]:
            pairs = [
                (float(row[first]), float(row[second_name]))
                for row in rows
                if row.get(first, "") != "" and row.get(second_name, "") != ""
            ]
            if len(pairs) < 5:
                continue
            first_values = [pair[0] for pair in pairs]
            second_values = [pair[1] for pair in pairs]
            value = _correlation(first_values, second_values)
            if math.isfinite(value):
                correlations.append(
                    {
                        "feature_a": first,
                        "feature_b": second_name,
                        "sample_count": len(pairs),
                        "pearson_r": value,
                        "absolute_r": abs(value),
                    }
                )
    correlations.sort(key=lambda row: float(row["absolute_r"]), reverse=True)
    return rows, correlations


def _status_counts(rows: list[dict[str, str]]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in rows:
        status = row.get("status") or "missing"
        counts[status] = counts.get(status, 0) + 1
    return counts


def _response_match_reason_counts(rows: list[dict[str, str]]) -> dict[str, int]:
    counts: dict[str, int] = {}
    for row in rows:
        reason = row.get("response_match_reason")
        if reason:
            counts[reason] = counts.get(reason, 0) + 1
    return counts


def _command_metrics(rows: list[dict[str, str]]) -> dict[str, Any]:
    response_count = sum(
        1
        for row in rows
        if row.get("response_missing", "").lower() not in {"true", "1"}
        and bool(row.get("status"))
    )
    return {
        "command_count": len(rows),
        "response_count": response_count,
        "missing_response_count": len(rows) - response_count,
        "invalid_response_timing_count": sum(
            1
            for row in rows
            if row.get("response_timing_valid", "").lower() in {"false", "0"}
            and row.get("response_missing", "").lower() not in {"true", "1"}
        ),
        "duplicate_response_count": sum(
            int(float(row.get("response_duplicate_count") or 0)) for row in rows
        ),
        "response_match_reason_counts": _response_match_reason_counts(rows),
        "response_before_write_complete_count": sum(
            1
            for value in _values(rows, "write_complete_to_response_us")
            if value < 0
        ),
        "response_fraction": response_count / len(rows) if rows else math.nan,
        "status_counts": _status_counts(rows),
        "queue_delay_us": _summary(_values(rows, "queue_delay_us")),
        "gatt_write_duration_us": _summary(
            _values(rows, "gatt_write_duration_us")
        ),
        "write_complete_to_response_us": _summary(
            _values(rows, "write_complete_to_response_us")
        ),
        "queue_to_response_rtt_us": _summary(
            _values(rows, "queue_to_response_rtt_us")
        ),
        "transport_rtt_us": _summary(_values(rows, "transport_rtt_us")),
        "clock_full_exchange_us": _summary(
            _values(rows, "clock_full_exchange_us")
        ),
        "clock_server_processing_us": _summary(
            _values(rows, "clock_server_processing_us")
        ),
        "clock_network_rtt_us": _summary(_values(rows, "clock_network_rtt_us")),
        "korora_receive_to_queue_us": _summary(
            _values(rows, "korora_receive_to_queue_us")
        ),
        "korora_queue_to_tx_start_us": _summary(
            _values(rows, "korora_queue_to_tx_start_us")
        ),
        "korora_notify_duration_us": _summary(
            _values(rows, "korora_notify_duration_us")
        ),
        "korora_receive_to_tx_complete_us": _summary(
            _values(rows, "korora_receive_to_tx_complete_us")
        ),
        "korora_att_mtu": _summary(_values(rows, "korora_att_mtu")),
        "korora_fragment_count": _summary(
            _values(rows, "korora_fragment_count")
        ),
    }


def _latency_diagnosis(rows: list[dict[str, str]]) -> dict[str, Any]:
    metrics = _command_metrics(rows)
    rtt = metrics["transport_rtt_us"].get("median")
    write = metrics["gatt_write_duration_us"].get("median")
    after_write = metrics["write_complete_to_response_us"].get("median")
    processing = metrics["clock_server_processing_us"].get("median")
    korora_total = metrics["korora_receive_to_tx_complete_us"].get("median")
    korora_receive_queue = metrics["korora_receive_to_queue_us"].get("median")
    korora_queue_tx = metrics["korora_queue_to_tx_start_us"].get("median")
    korora_notify = metrics["korora_notify_duration_us"].get("median")
    if rtt is None:
        verdict = "no_response_timing"
        explanation = "No matched command responses contain transport timing."
    elif korora_total is not None and korora_queue_tx is not None and (
        korora_queue_tx >= 0.5 * korora_total
    ):
        verdict = "korora_response_queue_dominates"
        explanation = (
            "Most measured Korora time is waiting for the BLE transmit worker. "
            "Inspect an in progress fragmented telemetry message and queue priority."
        )
    elif korora_total is not None and korora_notify is not None and (
        korora_notify >= 0.5 * korora_total
    ):
        verdict = "korora_notification_dominates"
        explanation = (
            "Most measured Korora time is submitting and completing BLE notification "
            "fragments. Check ATT MTU, response fragment count, and connection interval."
        )
    elif write is None:
        verdict = "legacy_log_missing_write_completion"
        explanation = (
            "The total is measurable, but this recording predates the GATT write "
            "completion timestamp. Re record to split the host write from the response path."
        )
    elif write >= max(0.0, after_write or 0.0):
        verdict = "host_gatt_write_dominates"
        explanation = (
            "Most host observed latency is inside Bleak write_gatt_char. Investigate "
            "BlueZ scheduling, connection parameters, and write acknowledgement latency."
        )
    elif processing is not None and processing >= 0.5 * max(0.0, after_write or 0.0):
        verdict = "korora_processing_dominates_response_path"
        explanation = (
            "Korora command processing accounts for most of the post write response path."
        )
    else:
        verdict = "response_delivery_path_dominates"
        explanation = (
            "Most latency occurs after the host GATT write returns and is not Korora clock "
            "command processing. Inspect Korora outbound queueing and BLE notification delivery."
        )
    return {
        "verdict": verdict,
        "explanation": explanation,
        "median_transport_rtt_us": rtt,
        "median_gatt_write_duration_us": write,
        "median_write_complete_to_response_us": after_write,
        "median_korora_processing_us": processing,
        "median_korora_receive_to_tx_complete_us": korora_total,
        "median_korora_receive_to_queue_us": korora_receive_queue,
        "median_korora_queue_to_tx_start_us": korora_queue_tx,
        "median_korora_notify_duration_us": korora_notify,
    }


def _correlation(first: list[float], second: list[float]) -> float:
    pairs = [
        (x, y)
        for x, y in zip(first, second)
        if math.isfinite(x) and math.isfinite(y)
    ]
    if len(pairs) < 2:
        return math.nan
    xs = np.array([pair[0] for pair in pairs], dtype=np.float64)
    ys = np.array([pair[1] for pair in pairs], dtype=np.float64)
    if np.ptp(xs) == 0 or np.ptp(ys) == 0:
        return math.nan
    return float(np.corrcoef(xs, ys)[0, 1])


def _rolling_prediction_errors(
    rows: list[dict[str, str]], window: int = 16
) -> list[dict[str, Any]]:
    by_source: dict[tuple[int, str], list[tuple[int, int]]] = {}
    for row in rows:
        source = int(row["source"])
        source_node = _node_name(row)
        local = _number(row, "actual_ticks")
        reference = _number(row, "requested_ticks")
        if local is None or reference is None:
            continue
        by_source.setdefault((source, source_node), []).append(
            (int(local), int(reference))
        )

    output: list[dict[str, Any]] = []
    for (source, source_node), points in by_source.items():
        for index in range(window, len(points)):
            training = points[index - window: index]
            x_ref = training[-1][0]
            y_ref = training[-1][1]
            x = np.array([point[0] - x_ref for point in training],
                         dtype=np.float64)
            y = np.array([point[1] - y_ref for point in training],
                         dtype=np.float64)
            slope, relative_intercept = np.polyfit(x, y, 1)
            fitted = relative_intercept + slope * x
            residual_ticks = fitted - y
            residual_ns = residual_ticks * 62.5
            local_deltas = np.diff(
                np.array([point[0] for point in training], dtype=np.float64)
            )
            reference_deltas = np.diff(
                np.array([point[1] for point in training], dtype=np.float64)
            )
            valid_intervals = (local_deltas > 0) & (reference_deltas > 0)
            interval_ppm = (
                (local_deltas[valid_intervals] /
                 reference_deltas[valid_intervals] - 1.0)
                * 1e6
            )
            predicted = y_ref + relative_intercept + \
                slope * (points[index][0] - x_ref)
            error_ticks = predicted - points[index][1]
            output.append(
                {
                    "source": source,
                    "source_node": source_node,
                    "index": index,
                    "window_size": window,
                    "window_span_s": (training[-1][1] - training[0][1])
                    / 16_000_000,
                    "local_ticks": points[index][0],
                    "reference_ticks": points[index][1],
                    "prediction_error_ticks": error_ticks,
                    "prediction_error_ns": error_ticks * 62.5,
                    "slope": slope,
                    "slope_error_ppm": (slope - 1.0) * 1e6,
                    "rolling_rms_ns": float(np.sqrt(np.mean(np.square(residual_ns)))),
                    "rolling_max_abs_residual_ns": float(
                        np.max(np.abs(residual_ns))
                    ),
                    "clock_instability_ppm": (
                        float(np.std(interval_ppm, ddof=1))
                        if len(interval_ppm) > 1
                        else 0.0
                    ),
                }
            )
    return output


def _ttl_results(rows: list[dict[str, str]]) -> list[dict[str, Any]]:
    sequences: dict[tuple[int, int], dict[str, Any]] = {}
    for row in rows:
        sequence_value = _number(row, "sequence")
        session = int(row.get("session_id") or 0)
        if sequence_value is None:
            continue
        key = (session, int(sequence_value))
        current = sequences.setdefault(
            key, {"session_id": session, "sequence": int(sequence_value)}
        )
        record_type = row["record_type"]
        requested = _number(row, "requested_ticks")
        actual = _number(row, "actual_ticks")
        if record_type == "ttl_scheduled":
            current["target_korora_ticks"] = requested or _number(
                row, "timestamp_ticks"
            )
        elif record_type == "ttl_generated":
            current["generated_local_ticks"] = actual or _number(
                row, "timestamp_ticks"
            )
            current["generated_korora_ticks"] = _number(
                row, "reference_ticks"
            )
        elif record_type == "ttl_captured":
            current["captured_korora_ticks"] = actual or _number(
                row, "timestamp_ticks"
            )
        elif record_type == "ttl_result":
            current["total_error_ns"] = _number(row, "value")
            current["status"] = row.get("status")

    output = list(sequences.values())
    for value in output:
        target = value.get("target_korora_ticks")
        generated = value.get("generated_korora_ticks")
        captured = value.get("captured_korora_ticks")
        if target is not None and generated is not None:
            value["generation_error_ns"] = (generated - target) * 62.5
        if generated is not None and captured is not None:
            value["capture_after_generation_ns"] = (
                captured - generated) * 62.5
        if "total_error_ns" not in value:
            if target is not None and captured is not None:
                value["total_error_ns"] = (captured - target) * 62.5
    return output


def analyse(parsed_directory: Path, output_directory: Path | None = None) -> dict[str, Any]:
    output_directory = output_directory or parsed_directory
    output_directory.mkdir(parents=True, exist_ok=True)
    record_rows = _read_csv(parsed_directory / "records.csv")
    quality_rows = _read_csv(parsed_directory / "sync_quality.csv")
    pair_rows = _read_csv(parsed_directory / "sync_pairs.csv")
    ttl_rows = _read_csv(parsed_directory / "ttl.csv")
    event_rows = _read_csv(parsed_directory / "events.csv")
    health_rows = _read_csv(parsed_directory / "health.csv")
    host_rows = _read_csv(parsed_directory / "host_transport_health.csv")
    command_rows = _read_csv(parsed_directory / "commands.csv")
    command_result_rows = _read_csv(parsed_directory / "command_results.csv")

    all_timestamps = [
        timestamp
        for rows in (
            record_rows,
            quality_rows,
            pair_rows,
            ttl_rows,
            event_rows,
            health_rows,
            host_rows,
            command_rows,
        )
        for row in rows
        if (timestamp := _timestamp_ns(row)) is not None
    ]
    origin_ns = min(all_timestamps, default=0)
    end_ns = max(all_timestamps, default=origin_ns)

    counter_deltas = _counter_delta_rows(health_rows, host_rows, origin_ns)
    counter_delta_columns = [
        "received_monotonic_ns",
        "elapsed_s",
        "source_node",
        "metric",
        "previous",
        "current",
        "delta",
        "interval_s",
        "rate_per_s",
        "counter_reset",
    ]
    _write_rows(
        output_directory / "counter_deltas.csv",
        counter_delta_columns,
        counter_deltas,
    )

    record_gaps = _record_gap_rows(record_rows, origin_ns)
    _write_rows(
        output_directory / "record_gaps.csv",
        [
            "source_node",
            "previous_monotonic_ns",
            "received_monotonic_ns",
            "start_elapsed_s",
            "end_elapsed_s",
            "gap_s",
            "median_interval_s",
            "gap_factor",
            "anomaly_threshold_s",
            "is_anomaly",
        ],
        record_gaps,
    )

    record_types = _record_type_rows(record_rows)
    _write_rows(
        output_directory / "record_type_summary.csv",
        [
            "source_node",
            "record_type",
            "count",
            "duration_s",
            "rate_hz",
            "median_interval_s",
            "p95_interval_s",
            "maximum_interval_s",
        ],
        record_types,
    )

    anomalies = _anomaly_rows(
        counter_deltas, record_gaps, command_rows, host_rows, origin_ns
    )
    _write_rows(
        output_directory / "anomaly_intervals.csv",
        [
            "anomaly_id",
            "category",
            "source_node",
            "metric",
            "start_s",
            "end_s",
            "duration_s",
            "delta",
            "peak_rate_per_s",
            "detail",
            "phase_268_435456_s",
            "phase_512_s",
            "phase_1024_s",
        ],
        anomalies,
    )

    timeseries, correlations = _build_timeseries(
        record_rows,
        quality_rows,
        health_rows,
        host_rows,
        command_rows,
        counter_deltas,
        origin_ns,
    )
    timeseries_columns = ["elapsed_s"] + sorted(
        {key for row in timeseries for key in row if key != "elapsed_s"}
    )
    _write_rows(
        output_directory / "timeseries_1s.csv", timeseries_columns, timeseries
    )
    _write_rows(
        output_directory / "correlations.csv",
        ["feature_a", "feature_b", "sample_count", "pearson_r", "absolute_r"],
        correlations,
    )

    node_names = sorted(
        {
            _node_name(row)
            for rows in (record_rows, quality_rows, pair_rows, event_rows, health_rows)
            for row in rows
            if row.get("source")
        }
    )
    node_summaries: dict[str, Any] = {}
    for node_name in node_names:
        records = [row for row in record_rows if _node_name(row) == node_name]
        quality = [row for row in quality_rows if _node_name(row) == node_name]
        pairs = [row for row in pair_rows if _node_name(row) == node_name]
        events = [row for row in event_rows if _node_name(row) == node_name]
        health = [row for row in health_rows if _node_name(row) == node_name]
        identity_row = next(
            iter(records or quality or pairs or events or health), {})
        address = _node_address(identity_row)
        received_times = sorted(_values(records, "received_monotonic_ns"))
        duration_s = (
            (received_times[-1] - received_times[0]) / 1e9
            if len(received_times) >= 2
            else 0.0
        )
        clocked_records = sorted(
            [
                row
                for row in records
                if _number(row, "received_monotonic_ns") is not None
                and _number(row, "timestamp_ticks") is not None
                and (_number(row, "clock_hz") or 0) > 0
            ],
            key=lambda row: int(float(row["received_monotonic_ns"])),
        )
        delivery_residual_ms: list[float] = []
        if clocked_records:
            first = clocked_records[0]
            first_host = int(float(first["received_monotonic_ns"]))
            first_local = float(
                first["timestamp_ticks"]) / float(first["clock_hz"])
            delivery_residual_ms = [
                (
                    (
                        float(row["timestamp_ticks"]) / float(row["clock_hz"])
                        - first_local
                    )
                    - (int(float(row["received_monotonic_ns"])
                           ) - first_host) / 1e9
                )
                * 1000
                for row in clocked_records
            ]
        record_type_counts: dict[str, int] = {}
        for row in records:
            record_type = row.get("record_type", "unknown")
            record_type_counts[record_type] = record_type_counts.get(
                record_type, 0) + 1

        missing_record_ids = 0
        duplicate_record_ids = 0
        record_id_resets = 0
        previous_id: int | None = None
        for row in sorted(
            records, key=lambda value: int(
                value.get("received_monotonic_ns") or 0)
        ):
            current_id = _integer(row, "record_id")
            if current_id is None:
                continue
            if previous_id is not None:
                difference = current_id - previous_id
                if difference > 1:
                    missing_record_ids += difference - 1
                elif difference == 0:
                    duplicate_record_ids += 1
                elif difference < 0:
                    record_id_resets += 1
            previous_id = current_id

        interval_error_ppm: list[float] = []
        previous_pair: tuple[float, float] | None = None
        for row in sorted(
            pairs, key=lambda value: int(
                value.get("received_monotonic_ns") or 0)
        ):
            local = _number(row, "actual_ticks")
            reference = _number(row, "requested_ticks")
            if local is None or reference is None:
                continue
            if previous_pair is not None:
                local_delta = local - previous_pair[0]
                reference_delta = reference - previous_pair[1]
                if local_delta > 0 and reference_delta > 0:
                    interval_error_ppm.append(
                        (local_delta / reference_delta - 1.0) * 1e6
                    )
            previous_pair = (local, reference)

        synchronized_count = sum(
            1 for row in quality if (int(float(row.get("flags") or 0)) & 0x04) != 0
        )
        model_generations = {
            int(value)
            for row in quality
            if (value := _number(row, "model_generation")) is not None
        }
        node_summaries[node_name] = {
            "logical_address": address,
            "logical_address_hex": f"0x{address:02x}" if address is not None else "",
            "fairy_index": identity_row.get("source_fairy_index", ""),
            "label": identity_row.get("source_label", "") or node_name,
            "duration_s": duration_s,
            "record_count": len(records),
            "record_rate_hz": len(records) / duration_s if duration_s > 0 else 0.0,
            "record_type_counts": record_type_counts,
            "delivery_time_residual_ms": _summary(delivery_residual_ms),
            "maximum_delivery_lag_excursion_ms": (
                statistics.median(delivery_residual_ms) -
                min(delivery_residual_ms)
                if delivery_residual_ms
                else math.nan
            ),
            "missing_record_id_count": missing_record_ids,
            "duplicate_record_id_count": duplicate_record_ids,
            "record_id_reset_count": record_id_resets,
            "pair_count": len(pairs),
            "quality_records": len(quality),
            "synchronized_quality_count": synchronized_count,
            "track_fraction": synchronized_count / len(quality) if quality else math.nan,
            "model_generation_count": len(model_generations),
            "sync_rms_ns": _summary(_values(quality, "rms_ns")),
            "skew_ppb": _summary(_values(quality, "skew_ppb")),
            "interval_error_ppb": _summary(
                _values(quality, "interval_error_ppb")
            ),
            "pair_interval_error_ppm": _summary(interval_error_ppm),
            "model_points": _summary(_values(quality, "model_points")),
            "event_count": len(events),
            "event_type_counts": {
                event_type: sum(1 for row in events if row.get(
                    "record_type") == event_type)
                for event_type in sorted({row.get("record_type", "") for row in events})
            },
            "maximum_dropped_records": max(
                [int(value) for value in _values(health, "dropped_records")],
                default=0,
            ),
            "maximum_transport_errors": max(
                [int(value) for value in _values(health, "transport_errors")],
                default=0,
            ),
            "maximum_queue_depth": max(
                [int(value) for value in _values(health, "queue_depth")], default=0
            ),
            "maximum_retries": max(
                [int(value) for value in _values(health, "retry_count")], default=0
            ),
            "rssi_dbm": _summary(_values(health, "rssi_dbm")),
        }

    prediction_rows = _rolling_prediction_errors(pair_rows)
    prediction_path = output_directory / "prediction_errors.csv"
    with prediction_path.open("w", newline="", encoding="utf-8") as handle:
        columns = [
            "source",
            "source_node",
            "index",
            "window_size",
            "window_span_s",
            "local_ticks",
            "reference_ticks",
            "prediction_error_ticks",
            "prediction_error_ns",
            "slope",
            "slope_error_ppm",
            "rolling_rms_ns",
            "rolling_max_abs_residual_ns",
            "clock_instability_ppm",
        ]
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        writer.writerows(prediction_rows)

    ttl_results = _ttl_results(ttl_rows)
    ttl_path = output_directory / "ttl_results.csv"
    ttl_columns = sorted(
        {key for result in ttl_results for key in result}
        or {"session_id", "sequence", "total_error_ns"}
    )
    with ttl_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=ttl_columns)
        writer.writeheader()
        writer.writerows(ttl_results)

    event_counts: dict[str, int] = {}
    event_counts_by_node: dict[str, dict[str, int]] = {}
    for row in event_rows:
        event_counts[row["record_type"]] = event_counts.get(
            row["record_type"], 0) + 1
        node_counts = event_counts_by_node.setdefault(_node_name(row), {})
        node_counts[row["record_type"]] = node_counts.get(
            row["record_type"], 0) + 1

    for node_name, node in node_summaries.items():
        selected_predictions = [
            row for row in prediction_rows if row["source_node"] == node_name
        ]
        node_predictions = [
            abs(float(row["prediction_error_ns"])) for row in selected_predictions
        ]
        node["one_step_prediction_absolute_error_ns"] = _summary(
            node_predictions)
        node["rolling_fit_count"] = len(selected_predictions)
        node["rolling_rms_ns"] = _summary(
            [float(row["rolling_rms_ns"]) for row in selected_predictions]
        )
        node["rolling_max_abs_residual_ns"] = _summary(
            [
                float(row["rolling_max_abs_residual_ns"])
                for row in selected_predictions
            ]
        )
        node["rolling_slope_error_ppm"] = _summary(
            [float(row["slope_error_ppm"]) for row in selected_predictions]
        )
        node["clock_instability_ppm"] = _summary(
            [float(row["clock_instability_ppm"])
             for row in selected_predictions]
        )
        node["instability_rms_correlation"] = _correlation(
            [float(row["clock_instability_ppm"])
             for row in selected_predictions],
            [float(row["rolling_rms_ns"]) for row in selected_predictions],
        )

    commands_by_destination = {
        destination: _command_metrics(
            [row for row in command_rows if (
                row.get("destination_node") or "unknown") == destination]
        )
        for destination in sorted(
            {row.get("destination_node") or "unknown" for row in command_rows}
        )
    }
    commands_by_opcode = {
        opcode: _command_metrics(
            [row for row in command_rows if (
                row.get("opcode") or "unknown") == opcode]
        )
        for opcode in sorted({row.get("opcode") or "unknown" for row in command_rows})
    }
    clock_command_rows = [
        row for row in command_rows if row.get("opcode") == "clock_exchange"
    ]
    control_command_rows = [
        row for row in command_rows if row.get("opcode") != "clock_exchange"
    ]
    command_results_by_node = {
        node: {
            "result_count": len(rows),
            "status_counts": _status_counts(rows),
            "result_receive_latency_us": _summary(
                _values(rows, "result_receive_latency_us")
            ),
        }
        for node in sorted(
            {row.get("source_node") or "unknown" for row in command_result_rows}
        )
        if (rows := [
            row
            for row in command_result_rows
            if (row.get("source_node") or "unknown") == node
        ])
    }

    node_summary_path = output_directory / "node_summary.csv"
    node_columns = [
        "node",
        "logical_address",
        "logical_address_hex",
        "fairy_index",
        "label",
        "duration_s",
        "record_count",
        "record_rate_hz",
        "maximum_delivery_lag_excursion_ms",
        "pair_count",
        "quality_records",
        "track_fraction",
        "sync_rms_median_us",
        "sync_rms_p95_us",
        "skew_median_ppm",
        "pair_interval_error_median_ppm",
        "prediction_error_median_us",
        "rolling_rms_median_us",
        "rolling_max_abs_residual_p95_us",
        "clock_instability_median_ppm",
        "instability_rms_correlation",
        "event_count",
        "missing_record_id_count",
        "duplicate_record_id_count",
        "maximum_queue_depth",
        "maximum_dropped_records",
        "maximum_transport_errors",
        "maximum_retries",
    ]
    with node_summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=node_columns)
        writer.writeheader()
        for node_name, node in node_summaries.items():
            writer.writerow(
                {
                    "node": node_name,
                    **{
                        key: node.get(key, "")
                        for key in node_columns
                        if key != "node"
                    },
                    "sync_rms_median_us": node["sync_rms_ns"].get("median", math.nan) / 1000,
                    "sync_rms_p95_us": node["sync_rms_ns"].get("p95", math.nan) / 1000,
                    "skew_median_ppm": node["skew_ppb"].get("median", math.nan) / 1000,
                    "pair_interval_error_median_ppm": node[
                        "pair_interval_error_ppm"
                    ].get("median", math.nan),
                    "prediction_error_median_us": node[
                        "one_step_prediction_absolute_error_ns"
                    ].get("median", math.nan)
                    / 1000,
                    "rolling_rms_median_us": node["rolling_rms_ns"].get(
                        "median", math.nan
                    )
                    / 1000,
                    "rolling_max_abs_residual_p95_us": node[
                        "rolling_max_abs_residual_ns"
                    ].get("p95", math.nan)
                    / 1000,
                    "clock_instability_median_ppm": node[
                        "clock_instability_ppm"
                    ].get("median", math.nan),
                }
            )

    command_summary_path = output_directory / "command_summary.csv"
    command_summary_columns = [
        "group",
        "value",
        "command_count",
        "response_count",
        "missing_response_count",
        "invalid_response_timing_count",
        "duplicate_response_count",
        "response_fraction",
        "rtt_median_us",
        "rtt_p95_us",
        "rtt_p99_us",
        "rtt_maximum_us",
        "queue_delay_median_us",
        "gatt_write_median_us",
        "after_write_median_us",
        "network_rtt_median_us",
        "korora_receive_to_queue_median_us",
        "korora_queue_to_tx_median_us",
        "korora_notify_median_us",
        "korora_receive_to_tx_complete_median_us",
        "korora_att_mtu_median",
        "korora_fragment_count_median",
    ]
    command_groups = [
        ("overall", "all", _command_metrics(command_rows)),
        ("traffic_class", "clock_exchange", _command_metrics(clock_command_rows)),
        ("traffic_class", "control", _command_metrics(control_command_rows)),
        *(
            ("destination", name, metrics)
            for name, metrics in commands_by_destination.items()
        ),
        *(("opcode", name, metrics)
          for name, metrics in commands_by_opcode.items()),
    ]
    with command_summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=command_summary_columns)
        writer.writeheader()
        for group, value, metrics in command_groups:
            rtt = metrics["transport_rtt_us"]
            queue = metrics["queue_delay_us"]
            write = metrics["gatt_write_duration_us"]
            after_write = metrics["write_complete_to_response_us"]
            network = metrics["clock_network_rtt_us"]
            korora_receive_queue = metrics["korora_receive_to_queue_us"]
            korora_queue_tx = metrics["korora_queue_to_tx_start_us"]
            korora_notify = metrics["korora_notify_duration_us"]
            korora_total = metrics["korora_receive_to_tx_complete_us"]
            writer.writerow(
                {
                    "group": group,
                    "value": value,
                    "command_count": metrics["command_count"],
                    "response_count": metrics["response_count"],
                    "missing_response_count": metrics["missing_response_count"],
                    "invalid_response_timing_count": metrics[
                        "invalid_response_timing_count"
                    ],
                    "duplicate_response_count": metrics[
                        "duplicate_response_count"
                    ],
                    "response_fraction": metrics["response_fraction"],
                    "rtt_median_us": rtt.get("median", ""),
                    "rtt_p95_us": rtt.get("p95", ""),
                    "rtt_p99_us": rtt.get("p99", ""),
                    "rtt_maximum_us": rtt.get("maximum", ""),
                    "queue_delay_median_us": queue.get("median", ""),
                    "gatt_write_median_us": write.get("median", ""),
                    "after_write_median_us": after_write.get("median", ""),
                    "network_rtt_median_us": network.get("median", ""),
                    "korora_receive_to_queue_median_us": korora_receive_queue.get(
                        "median", ""
                    ),
                    "korora_queue_to_tx_median_us": korora_queue_tx.get(
                        "median", ""
                    ),
                    "korora_notify_median_us": korora_notify.get("median", ""),
                    "korora_receive_to_tx_complete_median_us": korora_total.get(
                        "median", ""
                    ),
                    "korora_att_mtu_median": metrics["korora_att_mtu"].get(
                        "median", ""
                    ),
                    "korora_fragment_count_median": metrics[
                        "korora_fragment_count"
                    ].get("median", ""),
                }
            )

    counter_summaries: dict[str, dict[str, Any]] = {}
    for row in counter_deltas:
        key = f"{row['source_node']}:{row['metric']}"
        current = counter_summaries.setdefault(
            key,
            {
                "source_node": row["source_node"],
                "metric": row["metric"],
                "total_increment": 0.0,
                "positive_sample_count": 0,
                "reset_count": 0,
                "rate_per_s": [],
            },
        )
        if bool(row["counter_reset"]):
            current["reset_count"] += 1
        else:
            current["total_increment"] += float(row["delta"])
            if float(row["delta"]) > 0:
                current["positive_sample_count"] += 1
                current["rate_per_s"].append(float(row["rate_per_s"]))
    for current in counter_summaries.values():
        current["positive_rate_per_s"] = _summary(current.pop("rate_per_s"))

    anomaly_counts = Counter(
        str(row["category"]) for row in anomalies
    )
    korora_drop_bursts = [
        row
        for row in anomalies
        if row["category"] == "record_loss_burst"
        and row["metric"] in {"korora_ble_dropped", "dropped_records"}
        and row["source_node"] in {"adelie_host", "korora"}
    ]
    if any(row["source_node"] == "adelie_host" for row in korora_drop_bursts):
        korora_drop_bursts = [
            row for row in korora_drop_bursts if row["source_node"] == "adelie_host"
        ]
    korora_drop_bursts.sort(key=lambda row: float(row["start_s"]))
    rtt_regime_changes = sorted(
        [
            row
            for row in anomalies
            if row["category"] == "command_rtt_regime_change"
        ],
        key=lambda row: float(row["start_s"]),
    )
    rtt_precursor_leads_s: list[float] = []
    for burst in korora_drop_bursts:
        prior = [
            change
            for change in rtt_regime_changes
            if 0.0
            <= float(burst["start_s"]) - float(change["start_s"])
            <= 180.0
            and float(change["delta"]) > 0
        ]
        if prior:
            closest = prior[-1]
            lead = float(burst["start_s"]) - float(closest["start_s"])
            burst["preceding_rtt_change_s"] = float(closest["start_s"])
            burst["rtt_change_to_drop_lead_s"] = lead
            burst["preceding_rtt_change_us"] = float(closest["delta"])
            rtt_precursor_leads_s.append(lead)
    burst_spacing_s = [
        float(later["start_s"]) - float(earlier["start_s"])
        for earlier, later in zip(korora_drop_bursts, korora_drop_bursts[1:])
    ]
    burst_starts = [float(row["start_s"]) for row in korora_drop_bursts]
    rollover_phase_tests = {
        "timer_32bit_268_435456_s": {
            "period_s": 268.435456,
            "minimum_covering_phase_span_s": _circular_phase_span(
                burst_starts, 268.435456
            ),
        },
        "rtc2_overflow_512_s": {
            "period_s": 512.0,
            "minimum_covering_phase_span_s": _circular_phase_span(
                burst_starts, 512.0
            ),
        },
        "two_rtc2_overflows_1024_s": {
            "period_s": 1024.0,
            "minimum_covering_phase_span_s": _circular_phase_span(
                burst_starts, 1024.0
            ),
        },
    }

    host_summary = {
        "sample_count": len(host_rows),
        "outgoing_queue": _summary(_values(host_rows, "outgoing_queue")),
        "last_rx_age_ms": _summary(_values(host_rows, "last_rx_age_ms")),
        "last_tx_age_ms": _summary(_values(host_rows, "last_tx_age_ms")),
        "clock_rms_us": _summary(_values(host_rows, "clock_rms_us")),
        "clock_rtt_us": _summary(_values(host_rows, "clock_last_rtt_us")),
        "clock_invalid_samples": sum(
            1
            for row in host_rows
            if str(row.get("clock_valid", "")).lower() in {"false", "0"}
        ),
        "last_errors": sorted(
            {row.get("last_error", "")
             for row in host_rows if row.get("last_error")}
        ),
    }

    summary = {
        "run": {
            "origin_monotonic_ns": origin_ns,
            "end_monotonic_ns": end_ns,
            "duration_s": (end_ns - origin_ns) / 1e9,
            "record_count": len(record_rows),
            "record_type_count": len(
                {row.get("record_type") for row in record_rows}
            ),
            "host_diagnostic_samples": len(host_rows),
        },
        "nodes": node_summaries,
        "one_step_prediction_error_ns": _summary(
            [abs(float(row["prediction_error_ns"])) for row in prediction_rows]
        ),
        "ttl_total_error_ns": _summary(
            [
                float(value["total_error_ns"])
                for value in ttl_results
                if value.get("total_error_ns") is not None
            ]
        ),
        "ttl_generation_error_ns": _summary(
            [
                float(value["generation_error_ns"])
                for value in ttl_results
                if value.get("generation_error_ns") is not None
            ]
        ),
        "ttl_capture_after_generation_ns": _summary(
            [
                float(value["capture_after_generation_ns"])
                for value in ttl_results
                if value.get("capture_after_generation_ns") is not None
            ]
        ),
        "ttl_stage_counts": {
            stage: sum(1 for value in ttl_results if value.get(
                stage) is not None)
            for stage in (
                "target_korora_ticks",
                "generated_local_ticks",
                "generated_korora_ticks",
                "captured_korora_ticks",
                "total_error_ns",
            )
        },
        "ttl_sequences": len(ttl_results),
        "event_counts": event_counts,
        "event_counts_by_node": event_counts_by_node,
        "record_types": record_types,
        "host_transport": host_summary,
        "counters": counter_summaries,
        "anomalies": {
            "count": len(anomalies),
            "counts_by_category": dict(anomaly_counts),
            "korora_ble_drop_bursts": korora_drop_bursts,
            "korora_ble_drop_burst_spacing_s": _summary(burst_spacing_s),
            "command_rtt_regime_changes": rtt_regime_changes,
            "rtt_increase_to_drop_lead_s": _summary(rtt_precursor_leads_s),
            "rollover_phase_tests": rollover_phase_tests,
            "strongest_correlations": correlations[:25],
        },
        "commands": {
            "overall": _command_metrics(command_rows),
            "by_destination": commands_by_destination,
            "by_opcode": commands_by_opcode,
            "clock_exchange": _command_metrics(clock_command_rows),
            "control": _command_metrics(control_command_rows),
            "results_by_node": command_results_by_node,
            "clock_exchange_latency_diagnosis": _latency_diagnosis(
                clock_command_rows
            ),
            "control_latency_diagnosis": _latency_diagnosis(
                control_command_rows
            ),
        },
    }
    summary_path = output_directory / "summary.json"
    summary_path.write_text(json.dumps(
        summary, indent=2) + "\n", encoding="utf-8")

    text_path = output_directory / "summary.txt"
    lines = ["Adelie static analysis", ""]
    for node_name, node in node_summaries.items():
        rms = node["sync_rms_ns"]
        lines.append(
            f"{node_name} ({node['logical_address_hex']}): records {node['record_count']}, "
            f"quality {node['quality_records']}, "
            f"median RMS {rms.get('median', math.nan) / 1000:.3f} us, "
            f"track {node['track_fraction']:.1%}, "
            f"dropped {node['maximum_dropped_records']}, "
            f"transport errors {node['maximum_transport_errors']}"
        )
    lines.extend(
        [
            "",
            f"Run duration: {summary['run']['duration_s']:.3f} s",
            f"Parsed records: {summary['run']['record_count']}",
            f"Detected anomaly intervals: {summary['anomalies']['count']}",
            f"Korora BLE drop bursts: {len(korora_drop_bursts)}",
        ]
    )
    for burst in korora_drop_bursts:
        lines.append(
            f"  t={float(burst['start_s']):.3f} to "
            f"{float(burst['end_s']):.3f} s, "
            f"dropped +{float(burst['delta']):.0f}, "
            f"peak {float(burst['peak_rate_per_s']):.2f}/s, "
            f"RTT rise lead "
            f"{float(burst.get('rtt_change_to_drop_lead_s', math.nan)):.2f} s, "
            f"RTC512 phase {float(burst['phase_512_s']):.3f} s"
        )
    if host_rows:
        lines.extend(
            [
                f"Host diagnostic samples: {host_summary['sample_count']}",
                f"Host outgoing queue maximum: "
                f"{host_summary['outgoing_queue'].get('maximum', math.nan):.0f}",
                f"Host last receive age p95: "
                f"{host_summary['last_rx_age_ms'].get('p95', math.nan):.3f} ms",
                f"Host clock invalid samples: "
                f"{host_summary['clock_invalid_samples']}",
            ]
        )
    command_summary = summary["commands"]["overall"]
    command_rtt = command_summary["transport_rtt_us"]
    clock_summary = summary["commands"]["clock_exchange"]
    control_summary = summary["commands"]["control"]
    clock_diagnosis = summary["commands"]["clock_exchange_latency_diagnosis"]
    control_diagnosis = summary["commands"]["control_latency_diagnosis"]
    ttl_summary = summary["ttl_total_error_ns"]
    lines.extend(
        [
            "",
            f"Commands: {command_summary['command_count']}",
            f"Command response fraction: {command_summary['response_fraction']:.1%}",
            f"Responses excluded from RTT timing: "
            f"{command_summary['invalid_response_timing_count']}",
            f"Duplicate matched responses: "
            f"{command_summary['duplicate_response_count']}",
            f"Median command transport RTT: {command_rtt.get('median', math.nan):.3f} us",
            f"P95 command transport RTT: {command_rtt.get('p95', math.nan):.3f} us",
            f"Background clock exchanges: {clock_summary['command_count']}",
            f"Median background clock exchange RTT: "
            f"{clock_summary['transport_rtt_us'].get('median', math.nan):.3f} us",
            f"Experiment control commands: {control_summary['command_count']}",
            f"Median experiment control RTT: "
            f"{control_summary['transport_rtt_us'].get('median', math.nan):.3f} us",
            f"Median host GATT write duration: "
            f"{command_summary['gatt_write_duration_us'].get('median', math.nan):.3f} us",
            f"Median GATT write completion to response: "
            f"{command_summary['write_complete_to_response_us'].get('median', math.nan):.3f} us",
            f"Median Korora clock command processing: "
            f"{clock_summary['clock_server_processing_us'].get('median', math.nan):.3f} us",
            f"Median Korora receive to response queue: "
            f"{clock_summary['korora_receive_to_queue_us'].get('median', math.nan):.3f} us",
            f"Median Korora response queue wait: "
            f"{clock_summary['korora_queue_to_tx_start_us'].get('median', math.nan):.3f} us",
            f"Median Korora notification duration: "
            f"{clock_summary['korora_notify_duration_us'].get('median', math.nan):.3f} us",
            f"Median Korora receive to notification completion: "
            f"{clock_summary['korora_receive_to_tx_complete_us'].get('median', math.nan):.3f} us",
            f"Median Adelie ATT MTU observed by Korora: "
            f"{clock_summary['korora_att_mtu'].get('median', math.nan):.0f} bytes",
            f"Median response fragment count: "
            f"{clock_summary['korora_fragment_count'].get('median', math.nan):.1f}",
            f"Clock exchange latency diagnosis: {clock_diagnosis['verdict']}",
            f"Clock exchange diagnosis detail: {clock_diagnosis['explanation']}",
            f"Control latency diagnosis: {control_diagnosis['verdict']}",
            f"Control diagnosis detail: {control_diagnosis['explanation']}",
            f"TTL sequences: {summary['ttl_sequences']}",
            f"TTL median total error: {ttl_summary.get('median', math.nan) / 1000:.3f} us",
            f"One step prediction median absolute error: "
            f"{summary['one_step_prediction_error_ns'].get('median', math.nan) / 1000:.3f} us",
        ]
    )
    text_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    report_lines = [
        "# System verification report",
        "",
        "## Run coverage",
        "",
        f"The parsed run covers {summary['run']['duration_s']:.3f} seconds and "
        f"contains {summary['run']['record_count']} Fairy records.",
        "",
        "## Korora outbound continuity",
        "",
    ]
    if korora_drop_bursts:
        report_lines.append(
            f"The analyser found {len(korora_drop_bursts)} Korora BLE outbound "
            "drop bursts. A rising Korora dropped records counter means the "
            "firmware was still executing but could not enqueue another record "
            "for Adelie or Galapagos. The firmware currently reports the sum of "
            "both endpoint drop counters, so this log cannot identify which BLE "
            "connection caused the loss. This is an outbound transport capacity "
            "problem, not by itself evidence of a CPU reset."
        )
        if rtt_precursor_leads_s:
            report_lines.append(
                " A sustained command RTT increase preceded "
                f"{len(rtt_precursor_leads_s)} of these bursts by a median of "
                f"{statistics.median(rtt_precursor_leads_s):.3f} seconds. This is "
                "consistent with service capacity falling below the offered record "
                "rate, followed by the outbound queue filling."
            )
        report_lines.extend(
            [
                "",
                "| Start s | End s | Duration s | Dropped | Peak per s | RTT rise lead s | RTC 512 phase s |",
                "| ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            ]
        )
        for burst in korora_drop_bursts:
            report_lines.append(
                f"| {float(burst['start_s']):.3f} | {float(burst['end_s']):.3f} | "
                f"{float(burst['duration_s']):.3f} | {float(burst['delta']):.0f} | "
                f"{float(burst['peak_rate_per_s']):.3f} | "
                f"{float(burst.get('rtt_change_to_drop_lead_s', math.nan)):.3f} | "
                f"{float(burst['phase_512_s']):.3f} |"
            )
    else:
        report_lines.append(
            "No positive Korora BLE dropped record burst was detected.")

    report_lines.extend(
        [
            "",
            "## Rollover alignment checks",
            "",
            "A small phase span means the bursts repeatedly start at a similar "
            "point in that hardware period. Two bursts are not enough to prove "
            "periodicity, so treat these as targeting evidence for a longer run.",
            "",
            "| Candidate period | Period s | Minimum phase span s |",
            "| --- | ---: | ---: |",
        ]
    )
    for name, values in rollover_phase_tests.items():
        span = values["minimum_covering_phase_span_s"]
        report_lines.append(
            f"| {name.replace('_', ' ')} | {values['period_s']:.6f} | "
            f"{span if math.isfinite(span) else math.nan:.6f} |"
        )

    report_lines.extend(
        [
            "",
            "## Node coverage",
            "",
            "| Node | Address | Records | Rate Hz | Longest gap s | Delivery lag excursion ms | Drops | Transport errors |",
            "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for node_name, node in node_summaries.items():
        node_gaps = [
            float(row["gap_s"])
            for row in record_gaps
            if row["source_node"] == node_name
        ]
        report_lines.append(
            f"| {node_name} | {node['logical_address_hex']} | {node['record_count']} | "
            f"{node['record_rate_hz']:.3f} | {max(node_gaps, default=math.nan):.3f} | "
            f"{node['maximum_delivery_lag_excursion_ms']:.3f} | "
            f"{node['maximum_dropped_records']} | {node['maximum_transport_errors']} |"
        )

    report_lines.extend(
        [
            "",
            "## Strong metric relationships",
            "",
            "These are correlations of one second bins. Correlation is diagnostic "
            "evidence and does not establish causation.",
            "",
            "| Metric A | Metric B | Samples | Pearson r |",
            "| --- | --- | ---: | ---: |",
        ]
    )
    for correlation in correlations[:20]:
        report_lines.append(
            f"| {correlation['feature_a']} | {correlation['feature_b']} | "
            f"{correlation['sample_count']} | {float(correlation['pearson_r']):.4f} |"
        )

    report_lines.extend(
        [
            "",
            "## Generated diagnostic tables",
            "",
            "- `anomaly_intervals.csv` detected burst, silence, reset and RTT intervals",
            "- `counter_deltas.csv` converts cumulative counters into increments and rates",
            "- `record_gaps.csv` contains every per node interarrival gap",
            "- `record_type_summary.csv` contains count and cadence by node and record type",
            "- `timeseries_1s.csv` aligns transport, clock, record and command metrics",
            "- `correlations.csv` ranks cross metric relationships",
            "- `host_transport_health.csv` preserves Adelie BLE diagnostics for new logs",
        ]
    )
    (output_directory / "verification_report.md").write_text(
        "\n".join(report_lines) + "\n", encoding="utf-8"
    )
    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyse parsed Adelie data")
    parser.add_argument("parsed_directory", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    summary = analyse(args.parsed_directory, args.output)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
