from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any, Iterable

import numpy as np
import pandas as pd

DEFAULT_HUB_HZ = 16_000_000.0
DEFAULT_LOCAL_HZ = 16_000_000.0
DEFAULT_SYNC_RATE_HZ = 4.0
DEFAULT_WINDOW_SIZE = 16
REFERENCE_NODE = "korora"
NODE_ALIASES = {"reward_port": "fairy"}
TRACK_STATES = {"TRACK", "LOCAL"}


def load_csv(path: Path) -> pd.DataFrame:
    if not path.exists():
        return pd.DataFrame()
    try:
        return pd.read_csv(path)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()


def numeric(frame: pd.DataFrame, columns: Iterable[str]) -> pd.DataFrame:
    frame = frame.copy()
    for column in columns:
        if column in frame:
            frame[column] = pd.to_numeric(frame[column], errors="coerce")
    return frame


def normalize_nodes(frame: pd.DataFrame) -> pd.DataFrame:
    if frame.empty or "node" not in frame:
        return frame
    frame = frame.copy()
    frame["node"] = frame["node"].replace(NODE_ALIASES)
    if "reference_node" in frame:
        frame["reference_node"] = frame["reference_node"].replace(NODE_ALIASES)
    return frame


def describe(values: pd.Series, prefix: str) -> dict[str, float | int]:
    clean = pd.to_numeric(values, errors="coerce").dropna()

    if clean.empty:
        return {
            f"{prefix}_count": 0,
            f"{prefix}_mean": math.nan,
            f"{prefix}_std": math.nan,
            f"{prefix}_min": math.nan,
            f"{prefix}_median": math.nan,
            f"{prefix}_p95": math.nan,
            f"{prefix}_p99": math.nan,
            f"{prefix}_max": math.nan,
        }

    return {
        f"{prefix}_count": int(len(clean)),
        f"{prefix}_mean": float(clean.mean()),
        f"{prefix}_std": float(clean.std(ddof=1)) if len(clean) > 1 else 0.0,
        f"{prefix}_min": float(clean.min()),
        f"{prefix}_median": float(clean.median()),
        f"{prefix}_p95": float(clean.quantile(0.95)),
        f"{prefix}_p99": float(clean.quantile(0.99)),
        f"{prefix}_max": float(clean.max()),
    }


def correlation(frame: pd.DataFrame, first: str, second: str) -> float:
    data = frame[[first, second]].dropna()
    if len(data) < 2:
        return math.nan
    if data[first].nunique() < 2 or data[second].nunique() < 2:
        return math.nan
    return float(data.corr().iloc[0, 1])


def derive_pairs(
    pairs: pd.DataFrame,
    hub_hz: float,
    local_hz: float,
) -> pd.DataFrame:
    pairs = normalize_nodes(pairs)
    pairs = numeric(
        pairs,
        [
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
        ],
    )

    if pairs.empty:
        return pairs

    pairs = pairs.dropna(subset=["node", "hub_ticks", "local_ticks"]).copy()
    pairs = pairs.drop_duplicates(
        subset=["node", "segment_id", "hub_ticks", "local_ticks"],
        keep="first",
    )
    pairs = pairs.sort_values(
        ["node", "segment_id", "hub_ticks", "source_line"]
    ).reset_index(drop=True)

    pairs["hub_time_s"] = pairs["hub_ticks"] / hub_hz
    pairs["elapsed_s"] = pairs["hub_time_s"] - pairs.groupby("node")[
        "hub_time_s"
    ].transform("min")
    pairs["transport_age_us"] = pairs["transport_age_ticks"] / hub_hz * 1e6

    grouped = pairs.groupby(["node", "segment_id"], sort=False)
    pairs["actual_hub_delta_ticks"] = grouped["hub_ticks"].diff()
    pairs["actual_local_delta_ticks"] = grouped["local_ticks"].diff()
    pairs["pulse_delta"] = grouped["pulse"].diff()
    pairs["hub_interval_s"] = pairs["actual_hub_delta_ticks"] / hub_hz
    pairs["local_interval_s"] = pairs["actual_local_delta_ticks"] / local_hz

    valid = (pairs["actual_hub_delta_ticks"] > 0) & (
        pairs["actual_local_delta_ticks"] > 0
    )

    pairs["local_interval_error_us"] = np.nan
    pairs.loc[valid, "local_interval_error_us"] = (
        pairs.loc[valid, "local_interval_s"] -
        pairs.loc[valid, "hub_interval_s"]
    ) * 1e6

    pairs["local_interval_error_ppm"] = np.nan
    pairs.loc[valid, "local_interval_error_ppm"] = (
        pairs.loc[valid, "local_interval_s"] /
        pairs.loc[valid, "hub_interval_s"] - 1.0
    ) * 1e6

    pairs["report_interval_ms"] = pairs["actual_hub_delta_ticks"] / hub_hz * 1000.0

    return pairs


def derive_sync(sync: pd.DataFrame, hub_hz: float) -> pd.DataFrame:
    sync = normalize_nodes(sync)
    sync = numeric(
        sync,
        [
            "pulse",
            "hub_ticks",
            "local_ticks",
            "status_flags",
            "record_flags",
            "pending_count",
            "slope_ppb",
            "local_reference_ticks",
            "hub_reference_ticks",
            "rms_ns",
            "prefit_residual_ns",
            "model_step_ns",
            "transport_age_ticks",
            "segment_id",
            "source_line",
        ],
    )

    if sync.empty:
        return sync

    sync = sync.dropna(subset=["node", "hub_ticks", "local_ticks"]).copy()
    sync = sync.sort_values(
        ["node", "segment_id", "hub_ticks", "source_line"]
    ).reset_index(drop=True)

    sync["hub_time_s"] = sync["hub_ticks"] / hub_hz
    sync["elapsed_s"] = sync["hub_time_s"] - sync.groupby("node")[
        "hub_time_s"
    ].transform("min")
    sync["slope_ppm"] = sync["slope_ppb"] / 1000.0
    sync["rms_us"] = sync["rms_ns"] / 1000.0
    sync["prefit_residual_us"] = sync["prefit_residual_ns"] / 1000.0
    sync["model_step_us"] = sync["model_step_ns"] / 1000.0
    sync["transport_age_us"] = sync["transport_age_ticks"] / hub_hz * 1e6
    sync["reference_minus_capture_us"] = (
        (sync["hub_reference_ticks"] - sync["hub_ticks"]) / hub_hz * 1e6
    )

    return sync


def derive_events(events: pd.DataFrame, hub_hz: float) -> pd.DataFrame:
    events = normalize_nodes(events)
    events = numeric(
        events,
        [
            "event_id",
            "local_ticks",
            "local_hz",
            "hub_ticks",
            "transport_age_ticks",
            "reference_event_id",
            "reference_hub_ticks",
            "error_ns",
            "matched",
            "segment_id",
            "source_line",
            "remote_sequence",
            "converted_hub_ticks",
            "matched_hub_sequence",
            "hub_capture_ticks",
        ],
    )

    if events.empty:
        return events

    # Accept the compatibility column names when analysing an older parsed set.
    if "event_id" not in events:
        if "remote_sequence" in events:
            events["event_id"] = events["remote_sequence"]
        else:
            events["event_id"] = np.nan

    if "hub_ticks" not in events:
        if "converted_hub_ticks" in events:
            events["hub_ticks"] = events["converted_hub_ticks"]
        else:
            events["hub_ticks"] = np.nan

    if "reference_event_id" not in events:
        if "matched_hub_sequence" in events:
            events["reference_event_id"] = events["matched_hub_sequence"]
        else:
            events["reference_event_id"] = np.nan

    if "reference_hub_ticks" not in events:
        if "hub_capture_ticks" in events:
            events["reference_hub_ticks"] = events["hub_capture_ticks"]
        else:
            events["reference_hub_ticks"] = np.nan

    events = events.dropna(subset=["node", "local_ticks"]).copy()
    sort_columns = [
        column for column in ["source_line", "node", "event_id"] if column in events
    ]
    events = events.sort_values(sort_columns).reset_index(drop=True)

    explicit_match = (
        pd.to_numeric(events.get("matched", 0), errors="coerce").fillna(0) > 0
    )
    inferred_match = (
        events["reference_hub_ticks"].notna()
        & events["error_ns"].notna()
        & (events["reference_hub_ticks"] >= 0)
        & (events["hub_ticks"] >= 0)
    )
    events["matched"] = explicit_match | inferred_match
    events["is_reference"] = events["node"] == REFERENCE_NODE
    events["is_gpio_event"] = events["kind"] == "GPIO_RISE"

    # Preserve exactly what appeared in the input file for diagnostics.
    events["event_id_raw"] = events["event_id"]
    events["reference_event_id_raw"] = events["reference_event_id"]

    # In Korora stream data, event_id == 0 on an unmatched remote GPIO event means
    # that no real event identifier was available. It must not be treated as the
    # first real event.
    missing_remote_event_id = (
        events["is_gpio_event"]
        & ~events["is_reference"]
        & ~events["matched"]
        & events["event_id"].fillna(0).eq(0)
    )

    events.loc[
        missing_remote_event_id,
        "event_id",
    ] = np.nan

    # The same rule applies to a zero reference ID on an unmatched event.
    missing_reference_event_id = ~events["matched"] & events[
        "reference_event_id"
    ].fillna(0).eq(0)

    events.loc[
        missing_reference_event_id,
        "reference_event_id",
    ] = np.nan

    # This is the identifier to use on plots that include both matched and
    # unmatched events. It represents observation order, not protocol identity.
    events["event_order"] = events.groupby(
        "node", sort=False).cumcount().add(1)

    # Nullable integer columns write as blank rather than 0.0 in CSV output.
    events["event_id"] = events["event_id"].astype("Int64")
    events["reference_event_id"] = events["reference_event_id"].astype("Int64")
    events["event_order"] = events["event_order"].astype("Int64")

    events["error_us"] = events["error_ns"] / 1000.0
    events["abs_error_us"] = events["error_us"].abs()

    # Transport age is expressed in the source clock domain in schema v3.
    events["transport_age_us"] = np.nan
    valid_hz = events["local_hz"] > 0
    events.loc[valid_hz, "transport_age_us"] = (
        events.loc[valid_hz, "transport_age_ticks"]
        / events.loc[valid_hz, "local_hz"]
        * 1e6
    )
    events["transport_age_ms"] = events["transport_age_us"] / 1000.0

    events["recomputed_error_ticks"] = (
        events["hub_ticks"] - events["reference_hub_ticks"]
    )
    events["recomputed_error_ns"] = events["recomputed_error_ticks"] / hub_hz * 1e9
    events["error_consistency_ns"] = events["error_ns"] - \
        events["recomputed_error_ns"]
    events["error_plus_transport_us"] = events["error_us"] + \
        events["transport_age_us"]
    events["error_minus_transport_us"] = events["error_us"] - \
        events["transport_age_us"]

    # Assign a real hub-domain timestamp only when the event actually has one.
    #
    # Previously this used hub_ticks as an unconditional fallback.  Unmatched
    # UNSYNC/REMOTE rows normally carry -1 or 0 as a sentinel, so they became
    # finite times and appeared at elapsed time zero in plots.
    valid_reference_time = (
        events["matched"]
        & events["reference_hub_ticks"].notna()
        & (events["reference_hub_ticks"] >= 0)
    )
    valid_converted_time = (
        events["state"].isin(TRACK_STATES)
        & events["hub_ticks"].notna()
        & (events["hub_ticks"] >= 0)
    )

    events["reference_time_ticks"] = np.nan

    # A matched remote GPIO event is positioned using its Korora reference
    # capture.  This keeps all matched nodes on the same event timeline.
    events.loc[valid_reference_time, "reference_time_ticks"] = events.loc[
        valid_reference_time, "reference_hub_ticks"
    ]

    # Korora LOCAL events and converted-but-not-matched TRACK events still have
    # a meaningful hub-domain time.  They may be used for non-error plots.
    converted_without_reference = valid_converted_time & ~valid_reference_time
    events.loc[converted_without_reference, "reference_time_ticks"] = events.loc[
        converted_without_reference, "hub_ticks"
    ]

    events["hub_time_s"] = events["reference_time_ticks"] / hub_hz

    finite = events.loc[
        np.isfinite(events["hub_time_s"]),
        "hub_time_s",
    ]
    events["elapsed_s"] = np.nan
    if not finite.empty:
        events.loc[np.isfinite(events["hub_time_s"]), "elapsed_s"] = events.loc[
            np.isfinite(events["hub_time_s"]), "hub_time_s"
        ] - float(finite.min())

    events["remote_sequence_delta"] = events.groupby("node", sort=False)[
        "event_id"
    ].diff()
    local_delta = events.groupby("node")["local_ticks"].diff()
    events["local_event_interval_ms"] = np.nan
    events.loc[valid_hz, "local_event_interval_ms"] = (
        local_delta[valid_hz] / events.loc[valid_hz, "local_hz"] * 1000.0
    )

    return events


def derive_ttl_pulses(ttl_records: pd.DataFrame, hub_hz: float) -> pd.DataFrame:
    ttl_records = normalize_nodes(ttl_records)
    ttl_records = numeric(
        ttl_records,
        [
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
        ],
    )

    if ttl_records.empty:
        return ttl_records

    ttl_records = ttl_records.dropna(subset=["sequence"]).copy()
    if "test_segment_id" not in ttl_records:
        ttl_records["test_segment_id"] = 0
    ttl_records["test_segment_id"] = ttl_records["test_segment_id"].fillna(0)
    ttl_records = ttl_records.sort_values(
        ["test_segment_id", "source_line"]
    ).reset_index(drop=True)

    rows: list[dict[str, Any]] = []

    for (segment_id, sequence), group in ttl_records.groupby(
        ["test_segment_id", "sequence"], sort=False
    ):
        def pick(column: str, preferred: tuple[str, ...]) -> float:
            if column not in group:
                return math.nan
            for record_type in preferred:
                values = group.loc[
                    group["record_type"] == record_type, column
                ].dropna()
                if not values.empty:
                    return float(values.iloc[-1])
            values = group[column].dropna()
            return float(values.iloc[-1]) if not values.empty else math.nan

        types = set(group["record_type"].astype(str))
        completed = "TTL_PULSE_RESULT" in types
        timed_out = "TTL_PULSE_TIMEOUT" in types

        target_hub_ticks = pick(
            "target_hub_ticks",
            (
                "TTL_PULSE_RESULT",
                "TTL_PULSE_ACQUIRED",
                "TTL_PULSE_GENERATED",
                "TTL_PULSE_SCHEDULED",
                "TTL_PULSE_TIMEOUT",
            ),
        )
        target_local_ticks = pick(
            "target_local_ticks",
            ("TTL_PULSE_RESULT", "TTL_PULSE_SCHEDULED"),
        )
        generated_hub_ticks = pick(
            "generated_hub_ticks",
            ("TTL_PULSE_RESULT", "TTL_PULSE_GENERATED"),
        )
        acquired_hub_ticks = pick(
            "acquired_hub_ticks",
            ("TTL_PULSE_RESULT", "TTL_PULSE_ACQUIRED"),
        )
        generation_error_ns = pick(
            "generation_error_ns",
            ("TTL_PULSE_RESULT", "TTL_PULSE_GENERATED"),
        )
        wire_offset_ns = pick("wire_offset_ns", ("TTL_PULSE_RESULT",))
        total_error_ns = pick(
            "total_error_ns",
            ("TTL_PULSE_RESULT", "TTL_PULSE_ACQUIRED"),
        )

        timeout_generated_seen = pick(
            "generated_seen", ("TTL_PULSE_TIMEOUT",)
        )
        timeout_acquired_seen = pick(
            "acquired_seen", ("TTL_PULSE_TIMEOUT",)
        )
        generated_seen = (
            math.isfinite(generated_hub_ticks)
            or (math.isfinite(timeout_generated_seen) and timeout_generated_seen > 0)
        )
        acquired_seen = (
            math.isfinite(acquired_hub_ticks)
            or (math.isfinite(timeout_acquired_seen) and timeout_acquired_seen > 0)
        )

        if completed:
            outcome = "RESULT"
        elif timed_out:
            outcome = "TIMEOUT"
        else:
            outcome = "INCOMPLETE"

        rows.append(
            {
                "test_segment_id": int(segment_id),
                "sequence": int(sequence),
                "outcome": outcome,
                "completed": completed,
                "timed_out": timed_out,
                "generated_seen": generated_seen,
                "acquired_seen": acquired_seen,
                "target_hub_ticks": target_hub_ticks,
                "target_local_ticks": target_local_ticks,
                "pulse_width_us": pick(
                    "pulse_width_us", ("TTL_PULSE_SCHEDULED",)
                ),
                "generated_local_ticks": pick(
                    "generated_local_ticks", ("TTL_PULSE_GENERATED",)
                ),
                "generated_hub_ticks": generated_hub_ticks,
                "acquired_hub_ticks": acquired_hub_ticks,
                "timeout_hub_ticks": pick(
                    "timeout_hub_ticks", ("TTL_PULSE_TIMEOUT",)
                ),
                "generation_error_ns": generation_error_ns,
                "wire_offset_ns": wire_offset_ns,
                "total_error_ns": total_error_ns,
                "first_source_line": int(group["source_line"].min()),
                "last_source_line": int(group["source_line"].max()),
                "record_count": int(len(group)),
            }
        )

    pulses = pd.DataFrame(rows)
    pulses = pulses.sort_values(
        ["test_segment_id", "first_source_line"]
    ).reset_index(drop=True)

    pulses["generation_error_us"] = pulses["generation_error_ns"] / 1000.0
    pulses["wire_offset_us"] = pulses["wire_offset_ns"] / 1000.0
    pulses["total_error_us"] = pulses["total_error_ns"] / 1000.0
    pulses["abs_generation_error_us"] = pulses["generation_error_us"].abs()
    pulses["abs_wire_offset_us"] = pulses["wire_offset_us"].abs()
    pulses["abs_total_error_us"] = pulses["total_error_us"].abs()

    pulses["target_time_s"] = pulses["target_hub_ticks"] / hub_hz
    finite_target = pulses.loc[
        np.isfinite(pulses["target_time_s"]), "target_time_s"
    ]
    pulses["elapsed_s"] = np.nan
    if not finite_target.empty:
        pulses.loc[np.isfinite(pulses["target_time_s"]), "elapsed_s"] = (
            pulses.loc[np.isfinite(pulses["target_time_s"]), "target_time_s"]
            - float(finite_target.min())
        )

    tick_to_ns = 1e9 / hub_hz
    pulses["recomputed_generation_error_ns"] = (
        pulses["generated_hub_ticks"] - pulses["target_hub_ticks"]
    ) * tick_to_ns
    pulses["recomputed_wire_offset_ns"] = (
        pulses["acquired_hub_ticks"] - pulses["generated_hub_ticks"]
    ) * tick_to_ns
    pulses["recomputed_total_error_ns"] = (
        pulses["acquired_hub_ticks"] - pulses["target_hub_ticks"]
    ) * tick_to_ns
    pulses["generation_error_consistency_ns"] = (
        pulses["generation_error_ns"] -
        pulses["recomputed_generation_error_ns"]
    )
    pulses["wire_offset_consistency_ns"] = (
        pulses["wire_offset_ns"] - pulses["recomputed_wire_offset_ns"]
    )
    pulses["total_error_consistency_ns"] = (
        pulses["total_error_ns"] - pulses["recomputed_total_error_ns"]
    )

    return pulses


def fit_affine(
    local: np.ndarray,
    hub: np.ndarray,
) -> tuple[float, float, np.ndarray]:
    local = local.astype(np.float64)
    hub = hub.astype(np.float64)
    local_reference = float(local[-1])
    hub_reference = float(hub[-1])
    x = local - local_reference
    y = hub - hub_reference
    x_mean = float(np.mean(x))
    y_mean = float(np.mean(y))
    centered_x = x - x_mean
    centered_y = y - y_mean
    denominator = float(np.dot(centered_x, centered_x))
    if denominator <= 0.0:
        raise ValueError("zero timestamp variance")
    slope = float(np.dot(centered_x, centered_y) / denominator)
    relative_intercept = y_mean - slope * x_mean
    intercept = hub_reference + relative_intercept - slope * local_reference
    predicted = intercept + slope * local
    residual = hub - predicted
    return slope, intercept, residual


def fit_window(
    window: pd.DataFrame,
    hub_hz: float,
    local_hz: float,
) -> dict[str, Any]:
    local = window["local_ticks"].to_numpy(dtype=np.float64)
    hub = window["hub_ticks"].to_numpy(dtype=np.float64)
    slope, intercept, residual_ticks = fit_affine(local, hub)
    residual_us = residual_ticks / hub_hz * 1e6

    local_deltas = np.diff(local)
    hub_deltas = np.diff(hub)
    valid = (local_deltas > 0) & (hub_deltas > 0)
    if not np.any(valid):
        raise ValueError("no positive intervals")

    interval_error_ppm = (
        (local_deltas[valid] / local_hz) / (hub_deltas[valid] / hub_hz) - 1.0
    ) * 1e6

    return {
        "node": str(window["node"].iloc[-1]),
        "segment_id": int(window["segment_id"].iloc[-1]),
        "start_pulse": int(window["pulse"].iloc[0]),
        "end_pulse": int(window["pulse"].iloc[-1]),
        "start_hub_ticks": int(window["hub_ticks"].iloc[0]),
        "end_hub_ticks": int(window["hub_ticks"].iloc[-1]),
        "start_elapsed_s": float(window["elapsed_s"].iloc[0]),
        "end_elapsed_s": float(window["elapsed_s"].iloc[-1]),
        "slope": slope,
        "slope_error_ppm": (slope - 1.0) * 1e6,
        "intercept_ticks": intercept,
        "rms_us": float(np.sqrt(np.mean(np.square(residual_us)))),
        "mean_residual_us": float(np.mean(residual_us)),
        "max_abs_residual_us": float(np.max(np.abs(residual_us))),
        "p95_abs_residual_us": float(np.percentile(np.abs(residual_us), 95)),
        "first_residual_us": float(residual_us[0]),
        "last_residual_us": float(residual_us[-1]),
        "clock_instability_ppm": (
            float(np.std(interval_error_ppm, ddof=1))
            if len(interval_error_ppm) > 1
            else 0.0
        ),
        "interval_error_ppm_mean": float(np.mean(interval_error_ppm)),
        "interval_error_ppm_min": float(np.min(interval_error_ppm)),
        "interval_error_ppm_max": float(np.max(interval_error_ppm)),
    }


def calculate_rolling_fits(
    pairs: pd.DataFrame,
    window_size: int,
    hub_hz: float,
    local_hz: float,
) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    if pairs.empty:
        return pd.DataFrame()

    for (_, _), segment in pairs.groupby(["node", "segment_id"], sort=True):
        segment = segment.sort_values(["hub_ticks", "source_line"]).reset_index(
            drop=True
        )
        if len(segment) < window_size:
            continue

        for end in range(window_size - 1, len(segment)):
            window = segment.iloc[end - window_size + 1: end + 1]
            local = window["local_ticks"].to_numpy()
            hub = window["hub_ticks"].to_numpy()
            if np.any(np.diff(local) <= 0) or np.any(np.diff(hub) <= 0):
                continue
            try:
                rows.append(fit_window(window, hub_hz, local_hz))
            except ValueError:
                continue

    return pd.DataFrame(rows)


def derive_adelie_clock(clock: pd.DataFrame, hub_hz: float) -> pd.DataFrame:
    clock = numeric(
        clock,
        [
            "sequence",
            "t1_adelie_ns",
            "t2_korora_ticks",
            "t3_korora_ticks",
            "t4_adelie_ns",
            "network_rtt_ns",
            "full_exchange_us",
            "midpoint_adelie_ns",
            "midpoint_korora_ticks",
        ],
    )
    if clock.empty:
        return clock

    clock = clock.dropna(
        subset=["t1_adelie_ns", "t2_korora_ticks", "t4_adelie_ns"]
    ).copy()
    clock = clock.sort_values(
        ["t1_adelie_ns", "sequence"]).reset_index(drop=True)

    if "t3_korora_ticks" not in clock or clock["t3_korora_ticks"].isna().all():
        clock["t3_korora_ticks"] = clock["t2_korora_ticks"]
    if "midpoint_adelie_ns" not in clock or clock["midpoint_adelie_ns"].isna().all():
        clock["midpoint_adelie_ns"] = (
            clock["t1_adelie_ns"] + clock["t4_adelie_ns"]
        ) / 2.0
    if (
        "midpoint_korora_ticks" not in clock
        or clock["midpoint_korora_ticks"].isna().all()
    ):
        clock["midpoint_korora_ticks"] = (
            clock["t2_korora_ticks"] + clock["t3_korora_ticks"]
        ) / 2.0

    clock["elapsed_s"] = (
        clock["midpoint_adelie_ns"] - float(clock["midpoint_adelie_ns"].min())
    ) / 1e9
    clock["server_processing_ticks"] = (
        clock["t3_korora_ticks"] - clock["t2_korora_ticks"]
    )
    clock["server_processing_us"] = clock["server_processing_ticks"] / hub_hz * 1e6
    clock["full_exchange_recomputed_us"] = (
        clock["t4_adelie_ns"] - clock["t1_adelie_ns"]
    ) / 1000.0
    clock["network_rtt_us"] = clock["network_rtt_ns"] / 1000.0
    missing_network = clock["network_rtt_us"].isna()
    clock.loc[missing_network, "network_rtt_us"] = (
        clock.loc[missing_network, "full_exchange_recomputed_us"]
        - clock.loc[missing_network, "server_processing_us"]
    )
    clock["sample_offset_ticks"] = (
        clock["midpoint_korora_ticks"] -
        (hub_hz / 1e9) * clock["midpoint_adelie_ns"]
    )

    return clock


def calculate_adelie_rolling_fits(
    clock: pd.DataFrame,
    window_size: int,
    hub_hz: float,
) -> pd.DataFrame:
    if clock.empty or len(clock) < window_size:
        return pd.DataFrame()

    rows: list[dict[str, Any]] = []
    expected_slope = hub_hz / 1e9

    for end in range(window_size - 1, len(clock)):
        window = clock.iloc[end - window_size + 1: end + 1]
        local = window["midpoint_adelie_ns"].to_numpy(dtype=np.float64)
        hub = window["midpoint_korora_ticks"].to_numpy(dtype=np.float64)

        try:
            slope, intercept, residual_ticks = fit_affine(local, hub)
        except ValueError:
            continue

        residual_us = residual_ticks / hub_hz * 1e6
        prediction_error_us = math.nan

        if end >= window_size:
            training = clock.iloc[end - window_size: end]
            try:
                previous_slope, previous_intercept, _ = fit_affine(
                    training["midpoint_adelie_ns"].to_numpy(dtype=np.float64),
                    training["midpoint_korora_ticks"].to_numpy(
                        dtype=np.float64),
                )
                predicted = previous_intercept + previous_slope * float(
                    clock.iloc[end]["midpoint_adelie_ns"]
                )
                prediction_error_us = (
                    (float(clock.iloc[end]
                     ["midpoint_korora_ticks"]) - predicted)
                    / hub_hz
                    * 1e6
                )
            except ValueError:
                pass

        rows.append(
            {
                "node": "adelie",
                "generation": end - window_size + 2,
                "window_size": window_size,
                "start_sequence": int(window["sequence"].iloc[0]),
                "end_sequence": int(window["sequence"].iloc[-1]),
                "start_elapsed_s": float(window["elapsed_s"].iloc[0]),
                "end_elapsed_s": float(window["elapsed_s"].iloc[-1]),
                "window_span_s": float(
                    window["elapsed_s"].iloc[-1] - window["elapsed_s"].iloc[0]
                ),
                "slope_ticks_per_ns": slope,
                "slope_error_ppm": (slope / expected_slope - 1.0) * 1e6,
                "intercept_ticks": intercept,
                "rms_us": float(np.sqrt(np.mean(np.square(residual_us)))),
                "mean_residual_us": float(np.mean(residual_us)),
                "max_abs_residual_us": float(np.max(np.abs(residual_us))),
                "p95_abs_residual_us": float(np.percentile(np.abs(residual_us), 95)),
                "current_residual_us": float(residual_us[-1]),
                "prediction_error_us": prediction_error_us,
                "network_rtt_us_min": float(window["network_rtt_us"].min()),
                "network_rtt_us_median": float(window["network_rtt_us"].median()),
                "network_rtt_us_p95": float(window["network_rtt_us"].quantile(0.95)),
                "full_exchange_us_median": float(
                    window["full_exchange_recomputed_us"].median()
                ),
            }
        )

    return pd.DataFrame(rows)


def derive_adelie_commands(commands: pd.DataFrame, hub_hz: float) -> pd.DataFrame:
    if commands.empty:
        return commands

    commands = commands.rename(
        columns={
            "reward_rx_hub_ticks": "fairy_rx_hub_ticks",
            "reward_exec_hub_ticks": "fairy_exec_hub_ticks",
            "reward_model_valid": "fairy_model_valid",
            "reward_action_us": "fairy_action_us",
            "korora_reward_rtt_us": "korora_fairy_rtt_us",
        }
    )

    commands = numeric(
        commands,
        [
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
        ],
    )

    commands = commands.dropna(subset=["sequence"]).copy()
    commands = commands.sort_values(
        ["adelie_t1_ns", "sequence"]).reset_index(drop=True)
    tick_to_us = 1e6 / hub_hz

    if "total_rtt_us" not in commands or commands["total_rtt_us"].isna().all():
        commands["total_rtt_us"] = (
            commands["adelie_t7_ns"] - commands["adelie_t1_ns"]
        ) / 1000.0

    recomputed = {
        "korora_queue_to_i2c_us": (
            commands["i2c_tx_start_ticks"] - commands["korora_rx_ticks"]
        )
        * tick_to_us,
        "korora_post_ack_us": (
            commands["korora_done_tx_ticks"] - commands["korora_ack_rx_ticks"]
        )
        * tick_to_us,
        "i2c_down_us": (commands["fairy_rx_hub_ticks"] - commands["i2c_tx_start_ticks"])
        * tick_to_us,
        "fairy_action_us": (
            commands["fairy_exec_hub_ticks"] - commands["fairy_rx_hub_ticks"]
        )
        * tick_to_us,
        "i2c_up_and_poll_us": (
            commands["korora_ack_rx_ticks"] - commands["fairy_exec_hub_ticks"]
        )
        * tick_to_us,
        "korora_fairy_rtt_us": (
            commands["korora_ack_rx_ticks"] - commands["i2c_tx_start_ticks"]
        )
        * tick_to_us,
        "korora_internal_us": (
            commands["korora_done_tx_ticks"] - commands["korora_rx_ticks"]
        )
        * tick_to_us,
    }

    for column, values in recomputed.items():
        if column not in commands:
            commands[column] = values
        else:
            commands[column] = commands[column].where(
                commands[column].notna(), values)

    minimum_t1 = commands["adelie_t1_ns"].dropna().min()
    commands["elapsed_s"] = (
        (commands["adelie_t1_ns"] - minimum_t1) / 1e9
        if pd.notna(minimum_t1)
        else np.nan
    )
    commands["ble_estimated_total_us"] = (
        commands["ble_down_est_us"] + commands["ble_up_est_us"]
    )
    commands["ble_direction_difference_us"] = (
        commands["ble_down_est_us"] - commands["ble_up_est_us"]
    )
    commands["host_and_ble_unaccounted_us"] = (
        commands["total_rtt_us"] - commands["korora_internal_us"]
    )
    commands["timeline_sum_us"] = (
        commands["ble_down_est_us"]
        + commands["korora_internal_us"]
        + commands["ble_up_est_us"]
    )
    commands["timeline_closure_us"] = (
        commands["total_rtt_us"] - commands["timeline_sum_us"]
    )
    commands["fairy_path_sum_us"] = (
        commands["i2c_down_us"]
        + commands["fairy_action_us"]
        + commands["i2c_up_and_poll_us"]
    )

    return commands


def create_adelie_command_events(commands: pd.DataFrame, hub_hz: float) -> pd.DataFrame:
    if commands.empty:
        return pd.DataFrame()

    rows: list[dict[str, Any]] = []
    tick_per_us = hub_hz / 1e6

    for command in commands.itertuples(index=False):
        sequence = int(command.sequence)
        t1_hub = (
            float(command.korora_rx_ticks)
            - float(command.ble_down_est_us) * tick_per_us
            if pd.notna(command.ble_down_est_us)
            else math.nan
        )
        t7_hub = (
            float(command.korora_done_tx_ticks)
            + float(command.ble_up_est_us) * tick_per_us
            if pd.notna(command.ble_up_est_us)
            else math.nan
        )

        stages = [
            ("adelie", "COMMAND_TX", command.adelie_t1_ns, 1e9, t1_hub, "TRACK"),
            (
                "korora",
                "COMMAND_RX",
                command.korora_rx_ticks,
                hub_hz,
                command.korora_rx_ticks,
                "LOCAL",
            ),
            (
                "korora",
                "COMMAND_FORWARD",
                command.i2c_tx_start_ticks,
                hub_hz,
                command.i2c_tx_start_ticks,
                "LOCAL",
            ),
            (
                "fairy",
                "COMMAND_RX",
                math.nan,
                hub_hz,
                command.fairy_rx_hub_ticks,
                "TRACK",
            ),
            (
                "fairy",
                "COMMAND_EXEC",
                math.nan,
                hub_hz,
                command.fairy_exec_hub_ticks,
                "TRACK",
            ),
            (
                "korora",
                "COMMAND_ACK_RX",
                command.korora_ack_rx_ticks,
                hub_hz,
                command.korora_ack_rx_ticks,
                "LOCAL",
            ),
            (
                "korora",
                "COMMAND_RESULT_TX",
                command.korora_done_tx_ticks,
                hub_hz,
                command.korora_done_tx_ticks,
                "LOCAL",
            ),
            ("adelie", "COMMAND_RESULT_RX",
             command.adelie_t7_ns, 1e9, t7_hub, "TRACK"),
        ]

        for node, kind, local_ticks, local_hz, hub_ticks, state in stages:
            elapsed_us = (
                (float(hub_ticks) - t1_hub) / hub_hz * 1e6
                if pd.notna(hub_ticks) and pd.notna(t1_hub)
                else math.nan
            )
            rows.append(
                {
                    "sequence": sequence,
                    "node": node,
                    "kind": kind,
                    "local_ticks": local_ticks,
                    "local_hz": local_hz,
                    "hub_ticks": hub_ticks,
                    "state": state,
                    "elapsed_us_from_command_tx": elapsed_us,
                }
            )

    return pd.DataFrame(rows)


def create_event_alignment(events: pd.DataFrame) -> pd.DataFrame:
    if events.empty:
        return pd.DataFrame()

    matched = events[
        events["is_gpio_event"]
        & events["matched"]
        & ~events["is_reference"]
        & events["event_id"].notna()
        & events["reference_event_id"].notna()
        & (events["event_id"] > 0)
        & (events["reference_event_id"] > 0)
    ].copy()
    if matched.empty:
        return pd.DataFrame()

    values = [
        "error_us",
        "transport_age_us",
        "hub_ticks",
        "reference_hub_ticks",
        "local_ticks",
        "event_id",
    ]
    pivoted: list[pd.DataFrame] = []

    for value in values:
        table = matched.pivot_table(
            index="reference_event_id",
            columns="node",
            values=value,
            aggfunc="first",
        )
        table.columns = [f"{value}_{column}" for column in table.columns]
        pivoted.append(table)

    alignment = pd.concat(pivoted, axis=1).reset_index()

    if {"error_us_fairy", "error_us_galapagos"}.issubset(alignment.columns):
        alignment["galapagos_minus_fairy_error_us"] = (
            alignment["error_us_galapagos"] - alignment["error_us_fairy"]
        )
    if {"hub_ticks_fairy", "hub_ticks_galapagos"}.issubset(alignment.columns):
        alignment["galapagos_minus_fairy_hub_ticks"] = (
            alignment["hub_ticks_galapagos"] - alignment["hub_ticks_fairy"]
        )

    return alignment.sort_values("reference_event_id").reset_index(drop=True)


def create_node_summary(
    pairs: pd.DataFrame,
    sync: pd.DataFrame,
    rolling: pd.DataFrame,
    diagnostics: pd.DataFrame,
    events: pd.DataFrame,
) -> pd.DataFrame:
    diagnostics = normalize_nodes(diagnostics)
    node_values: set[str] = set()
    for frame in (pairs, sync, diagnostics, events):
        if not frame.empty and "node" in frame:
            node_values |= set(frame["node"].dropna().astype(str))

    rows: list[dict[str, Any]] = []

    for node in sorted(node_values):
        # Adelie CSVs have a dedicated summary below. Serial EVENT rows from
        # Adelie are still retained in events_derived.csv, but must not create
        # a second, mostly empty summary row.
        if node == "adelie":
            continue

        node_pairs = (
            pairs[pairs["node"] == node].copy(
            ) if not pairs.empty else pd.DataFrame()
        )
        node_sync = (
            sync[sync["node"] == node].copy(
            ) if not sync.empty else pd.DataFrame()
        )
        node_rolling = (
            rolling[rolling["node"] == node].copy()
            if not rolling.empty
            else pd.DataFrame()
        )
        node_diagnostics = (
            diagnostics[diagnostics["node"] == node].copy()
            if not diagnostics.empty
            else pd.DataFrame()
        )
        node_events = (
            events[events["node"] == node].copy()
            if not events.empty
            else pd.DataFrame()
        )
        gpio_events = (
            node_events[node_events["is_gpio_event"]].copy()
            if not node_events.empty
            else pd.DataFrame()
        )

        row: dict[str, Any] = {
            "node": node,
            "source_type": "korora_stream",
            "pair_count": len(node_pairs),
            "sync_count": len(node_sync),
            "rolling_fit_count": len(node_rolling),
            "all_event_count": len(node_events),
            "external_event_count": len(gpio_events),
        }

        if node == REFERENCE_NODE:
            row["reference_event_count"] = len(gpio_events)
            rows.append(row)
            continue

        if not node_pairs.empty:
            row["duration_s"] = float(
                node_pairs["hub_time_s"].max() - node_pairs["hub_time_s"].min()
            )
            row["segment_count"] = int(node_pairs["segment_id"].nunique())
            row.update(
                describe(
                    node_pairs["local_interval_error_ppm"], "interval_error_ppm")
            )
            row.update(
                describe(node_pairs["transport_age_us"], "transport_age_us"))

        if not node_sync.empty:
            row["track_fraction"] = float(
                (node_sync["state"] == "TRACK").mean())
            row["track_sync_count"] = int(
                (node_sync["state"] == "TRACK").sum())
            row["acquire_sync_count"] = int(
                (node_sync["state"] == "ACQUIRE").sum())
            track = node_sync[node_sync["state"] == "TRACK"]
            if not track.empty:
                row.update(describe(track["slope_ppm"], "firmware_slope_ppm"))
                row.update(describe(track["rms_us"], "firmware_rms_us"))
                row.update(
                    describe(
                        track["prefit_residual_us"].abs(
                        ), "abs_prefit_residual_us"
                    )
                )
                row.update(
                    describe(track["model_step_us"].abs(), "abs_model_step_us"))

        if not node_rolling.empty:
            row.update(describe(node_rolling["rms_us"], "rolling_rms_us"))
            row.update(
                describe(
                    node_rolling["max_abs_residual_us"], "rolling_max_abs_residual_us"
                )
            )
            row.update(
                describe(
                    node_rolling["clock_instability_ppm"], "clock_instability_ppm")
            )
            corr = correlation(node_rolling, "clock_instability_ppm", "rms_us")
            if math.isfinite(corr):
                row["instability_rms_correlation"] = corr

        if not node_diagnostics.empty:
            for record_type, output_name in {
                "MODEL_RESET": "model_reset_count",
                "MODEL_STEP_REJECT": "model_step_reject_count",
                "FIT_REJECT": "fit_reject_count",
                "ADMISSION_REJECT": "admission_reject_count",
                "DUPLICATE_SYNC_IN_WINDOW": "duplicate_sync_count",
                "WINDOW_REJECT": "window_reject_count",
                "WINDOW_TIMEOUT": "window_timeout_count",
            }.items():
                row[output_name] = int(
                    (node_diagnostics["record_type"] == record_type).sum()
                )

        if not gpio_events.empty:
            matched = gpio_events[gpio_events["matched"]].copy()
            row["event_track_count"] = int(len(matched))
            row["event_unsync_count"] = int(len(gpio_events) - len(matched))
            if not matched.empty:
                row.update(describe(matched["error_us"], "event_error_us"))
                row.update(
                    describe(matched["abs_error_us"], "event_abs_error_us"))
                row.update(
                    describe(matched["transport_age_us"],
                             "event_transport_age_us")
                )
                row.update(
                    describe(
                        matched["error_plus_transport_us"],
                        "event_error_plus_transport_us",
                    )
                )
                row.update(
                    describe(
                        matched["error_consistency_ns"].abs(),
                        "event_abs_error_consistency_ns",
                    )
                )
                corr = correlation(matched, "error_us", "transport_age_us")
                if math.isfinite(corr):
                    row["event_error_transport_correlation"] = corr

        rows.append(row)

    return pd.DataFrame(rows)


def create_ttl_report_section(ttl_pulses: pd.DataFrame) -> pd.DataFrame:
    if ttl_pulses.empty:
        return pd.DataFrame()

    completed = ttl_pulses[ttl_pulses["completed"]].copy()
    row: dict[str, Any] = {
        "report_section": "scheduled_ttl",
        "node": "",
        "source_type": "scheduled_ttl_test",
        "path": "galapagos_to_korora",
        "ttl_test_count": int(len(ttl_pulses)),
        "ttl_result_count": int(ttl_pulses["completed"].sum()),
        "ttl_timeout_count": int(ttl_pulses["timed_out"].sum()),
        "ttl_incomplete_count": int(
            (~ttl_pulses["completed"] & ~ttl_pulses["timed_out"]).sum()
        ),
        "ttl_success_fraction": float(ttl_pulses["completed"].mean()),
        "ttl_generated_seen_fraction": float(ttl_pulses["generated_seen"].mean()),
        "ttl_acquired_seen_fraction": float(ttl_pulses["acquired_seen"].mean()),
    }

    pulse_width = pd.to_numeric(ttl_pulses["pulse_width_us"], errors="coerce")
    if pulse_width.notna().any():
        row.update(describe(pulse_width, "ttl_pulse_width_us"))

    if not completed.empty:
        row.update(
            describe(completed["generation_error_us"], "ttl_generation_error_us"))
        row.update(describe(completed["wire_offset_us"], "ttl_wire_offset_us"))
        row.update(describe(completed["total_error_us"], "ttl_total_error_us"))
        row.update(
            describe(completed["abs_total_error_us"], "ttl_abs_total_error_us")
        )
        row["ttl_negative_wire_offset_fraction"] = float(
            (completed["wire_offset_us"] < 0).mean()
        )
        row.update(
            describe(
                completed["generation_error_consistency_ns"].abs(),
                "ttl_abs_generation_consistency_ns",
            )
        )
        row.update(
            describe(
                completed["wire_offset_consistency_ns"].abs(),
                "ttl_abs_wire_consistency_ns",
            )
        )
        row.update(
            describe(
                completed["total_error_consistency_ns"].abs(),
                "ttl_abs_total_consistency_ns",
            )
        )

    return pd.DataFrame([row])


def create_adelie_summary(
    clock: pd.DataFrame,
    rolling: pd.DataFrame,
    commands: pd.DataFrame,
    window_size: int,
) -> pd.DataFrame:
    if clock.empty and commands.empty:
        return pd.DataFrame()

    row: dict[str, Any] = {
        "node": "adelie",
        "source_type": "adelie_csv",
        "clock_sample_count": len(clock),
        "rolling_fit_count": len(rolling),
        "command_count": len(commands),
        "model_window": window_size,
    }

    if not clock.empty:
        row["duration_s"] = float(
            clock["elapsed_s"].max() - clock["elapsed_s"].min())
        row["track_fraction"] = float(
            max(0, len(clock) - window_size + 1) / len(clock))
        row.update(describe(clock["network_rtt_us"], "clock_network_rtt_us"))
        row.update(
            describe(clock["full_exchange_recomputed_us"],
                     "clock_full_exchange_us")
        )
        row.update(
            describe(clock["server_processing_us"],
                     "clock_server_processing_us")
        )

    if not rolling.empty:
        row.update(
            describe(rolling["slope_error_ppm"], "clock_slope_error_ppm"))
        row.update(describe(rolling["rms_us"], "clock_rms_us"))
        row.update(
            describe(rolling["max_abs_residual_us"],
                     "clock_max_abs_residual_us")
        )
        row.update(
            describe(
                rolling["prediction_error_us"].abs(
                ), "clock_abs_prediction_error_us"
            )
        )
        row["model_generation_min"] = int(rolling["generation"].min())
        row["model_generation_max"] = int(rolling["generation"].max())

    if not commands.empty:
        for column, prefix in {
            "total_rtt_us": "command_total_rtt_us",
            "ble_down_est_us": "command_ble_down_est_us",
            "ble_up_est_us": "command_ble_up_est_us",
            "korora_internal_us": "command_korora_internal_us",
            "korora_queue_to_i2c_us": "command_korora_queue_to_i2c_us",
            "i2c_down_us": "command_i2c_down_us",
            "fairy_action_us": "command_fairy_action_us",
            "i2c_up_and_poll_us": "command_i2c_up_and_poll_us",
            "korora_post_ack_us": "command_korora_post_ack_us",
            "host_and_ble_unaccounted_us": "command_host_and_ble_us",
            "ble_direction_difference_us": "command_ble_direction_difference_us",
            "timeline_closure_us": "command_timeline_closure_us",
        }.items():
            if column in commands:
                row.update(describe(commands[column], prefix))
        if "fairy_model_valid" in commands:
            valid = pd.to_numeric(
                commands["fairy_model_valid"], errors="coerce")
            row["fairy_model_valid_fraction"] = float((valid == 1).mean())

    return pd.DataFrame([row])


def print_summary(summary: pd.DataFrame) -> str:
    """Print one integrated report containing node and cross-node sections."""
    lines: list[str] = []

    common_order = [
        "pair_count",
        "sync_count",
        "duration_s",
        "segment_count",
        "track_fraction",
        "model_reset_count",
        "fit_reject_count",
        "interval_error_ppm_mean",
        "interval_error_ppm_std",
        "firmware_slope_ppm_mean",
        "firmware_rms_us_median",
        "firmware_rms_us_p95",
        "rolling_rms_us_median",
        "rolling_rms_us_p95",
        "external_event_count",
        "event_track_count",
        "event_unsync_count",
        "event_error_us_median",
        "event_abs_error_us_median",
        "event_abs_error_us_p95",
        "event_transport_age_us_median",
        "event_error_transport_correlation",
    ]

    ttl_order = [
        "ttl_test_count",
        "ttl_result_count",
        "ttl_timeout_count",
        "ttl_incomplete_count",
        "ttl_success_fraction",
        "ttl_generated_seen_fraction",
        "ttl_acquired_seen_fraction",
        "ttl_pulse_width_us_median",
        "ttl_generation_error_us_median",
        "ttl_generation_error_us_p95",
        "ttl_wire_offset_us_median",
        "ttl_wire_offset_us_p95",
        "ttl_total_error_us_median",
        "ttl_total_error_us_p95",
        "ttl_abs_total_error_us_p95",
        "ttl_negative_wire_offset_fraction",
        "ttl_abs_generation_consistency_ns_max",
        "ttl_abs_wire_consistency_ns_max",
        "ttl_abs_total_consistency_ns_max",
    ]

    adelie_order = [
        "clock_sample_count",
        "duration_s",
        "model_window",
        "rolling_fit_count",
        "track_fraction",
        "clock_network_rtt_us_min",
        "clock_network_rtt_us_median",
        "clock_network_rtt_us_p95",
        "clock_slope_error_ppm_median",
        "clock_slope_error_ppm_p95",
        "clock_rms_us_median",
        "clock_rms_us_p95",
        "clock_abs_prediction_error_us_median",
        "clock_abs_prediction_error_us_p95",
        "command_count",
        "command_total_rtt_us_median",
        "command_total_rtt_us_p95",
        "command_ble_down_est_us_median",
        "command_ble_up_est_us_median",
        "command_korora_internal_us_median",
        "command_i2c_down_us_median",
        "command_fairy_action_us_median",
        "command_i2c_up_and_poll_us_median",
        "command_host_and_ble_us_median",
    ]

    lines.append("=" * 72)
    lines.append("Korora synchronization and scheduled TTL analysis")
    lines.append("=" * 72)

    for _, row in summary.iterrows():
        report_section = str(row.get("report_section", "") or "")
        source_type = str(row.get("source_type", "") or "")

        if report_section == "scheduled_ttl" or source_type == "scheduled_ttl_test":
            lines.append("")
            lines.append("=" * 72)
            lines.append("Scheduled TTL path: Galapagos to Korora")
            lines.append("=" * 72)
            for key in ttl_order:
                if key not in row or pd.isna(row[key]):
                    continue
                lines.append(f"{key:46s}: {row[key]}")
            continue

        node = str(row.get("node", ""))
        lines.append("")
        lines.append("=" * 72)
        lines.append(f"Node: {node}")
        lines.append("=" * 72)

        if node == REFERENCE_NODE:
            lines.append(
                f"reference_event_count                         : {int(row.get('reference_event_count', 0))}"
            )
            lines.append(
                "Korora is the reference clock; zero-error fit statistics are omitted."
            )
            continue

        order = adelie_order if node == "adelie" else common_order
        for key in order:
            if key not in row or pd.isna(row[key]):
                continue
            lines.append(f"{key:46s}: {row[key]}")

    text = "\n".join(lines) + "\n"
    print(text, end="")
    return text


def analyse_directory(
    *,
    input_dir: Path,
    output_dir: Path,
    hub_hz: float,
    local_hz: float,
    window_size: int,
) -> pd.DataFrame:
    output_dir.mkdir(parents=True, exist_ok=True)

    pairs = derive_pairs(load_csv(input_dir / "pairs.csv"), hub_hz, local_hz)
    sync = derive_sync(load_csv(input_dir / "sync.csv"), hub_hz)
    diagnostics = normalize_nodes(load_csv(input_dir / "diagnostics.csv"))
    events_path = input_dir / "events.csv"
    if not events_path.exists():
        events_path = input_dir / "external_events.csv"
    events = derive_events(load_csv(events_path), hub_hz)
    ttl_pulses = derive_ttl_pulses(
        load_csv(input_dir / "ttl_records.csv"), hub_hz
    )

    rolling = calculate_rolling_fits(pairs, window_size, hub_hz, local_hz)
    event_alignment = create_event_alignment(events)

    adelie_clock = derive_adelie_clock(
        load_csv(input_dir / "adelie_clock.csv"), hub_hz)
    adelie_rolling = calculate_adelie_rolling_fits(
        adelie_clock, window_size, hub_hz)
    adelie_commands = derive_adelie_commands(
        load_csv(input_dir / "adelie_commands.csv"), hub_hz
    )
    adelie_command_events = create_adelie_command_events(
        adelie_commands, hub_hz)

    node_summary = create_node_summary(
        pairs, sync, rolling, diagnostics, events)
    adelie_summary = create_adelie_summary(
        adelie_clock, adelie_rolling, adelie_commands, window_size
    )
    ttl_report_section = create_ttl_report_section(ttl_pulses)
    summary = pd.concat(
        [node_summary, adelie_summary, ttl_report_section],
        ignore_index=True,
        sort=False,
    )

    pairs.to_csv(output_dir / "pairs_derived.csv", index=False)
    sync.to_csv(output_dir / "sync_derived.csv", index=False)
    rolling.to_csv(output_dir / "rolling_fits.csv", index=False)
    events.to_csv(output_dir / "events_derived.csv", index=False)
    # Compatibility name.
    events.to_csv(output_dir / "external_events_derived.csv", index=False)
    event_alignment.to_csv(output_dir / "event_alignment.csv", index=False)
    ttl_pulses.to_csv(output_dir / "ttl_pulses_derived.csv", index=False)
    # There is deliberately no separate TTL summary.  Cross-node TTL metrics
    # are part of the single integrated summary.csv and summary.txt report.
    stale_ttl_summary = output_dir / "ttl_summary.csv"
    if stale_ttl_summary.exists():
        stale_ttl_summary.unlink()
    adelie_clock.to_csv(output_dir / "adelie_clock_derived.csv", index=False)
    adelie_rolling.to_csv(output_dir / "adelie_rolling_fits.csv", index=False)
    adelie_commands.to_csv(
        output_dir / "adelie_commands_derived.csv", index=False)
    adelie_command_events.to_csv(
        output_dir / "adelie_command_events.csv", index=False)
    summary.to_csv(output_dir / "summary.csv", index=False)

    summary_text = print_summary(summary)
    (output_dir / "summary.txt").write_text(summary_text, encoding="utf-8")

    metadata = {
        "hub_hz": hub_hz,
        "local_hz": local_hz,
        "window_size": window_size,
        "counts": {
            "pairs": len(pairs),
            "sync": len(sync),
            "events": len(events),
            "ttl_pulses": len(ttl_pulses),
            "adelie_clock": len(adelie_clock),
            "adelie_rolling": len(adelie_rolling),
            "adelie_commands": len(adelie_commands),
        },
    }
    (output_dir / "analysis_manifest.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )

    return summary


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyse normalized Korora and Adelie data."
    )
    parser.add_argument("--input", type=Path, default=Path("parsed"))
    parser.add_argument("--output", type=Path, default=Path("analysis"))
    parser.add_argument("--hub-hz", type=float, default=DEFAULT_HUB_HZ)
    parser.add_argument("--local-hz", type=float, default=DEFAULT_LOCAL_HZ)
    parser.add_argument("--sync-rate", type=float,
                        default=DEFAULT_SYNC_RATE_HZ)
    parser.add_argument("--window", type=int, default=DEFAULT_WINDOW_SIZE)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.window < 3:
        raise SystemExit("Window size must be at least 3")
    if args.hub_hz <= 0 or args.local_hz <= 0:
        raise SystemExit("Clock frequencies must be positive")

    summary = analyse_directory(
        input_dir=args.input,
        output_dir=args.output,
        hub_hz=args.hub_hz,
        local_hz=args.local_hz,
        window_size=args.window,
    )
    if summary.empty:
        raise SystemExit("No analysable Korora or Adelie data found")
    print(f"Results written to: {args.output}")


if __name__ == "__main__":
    main()
