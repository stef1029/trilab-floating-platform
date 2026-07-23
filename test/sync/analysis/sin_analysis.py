"""
Fit one shared sinusoidal period to Fairy clock-rate variation across many logs.

Each log is an independently started recording, so one absolute phase cannot be
shared unless the recordings have a common external time reference. The model is:

    rate_i(t) =
        offset_i
        + trend_i * (t - reference_i)
        + b_i * sin(2*pi*t / T)
        + d_i * cos(2*pi*t / T)

T is shared by every recording. Each log gets its own offset, linear trend,
amplitude, and phase.

Important: individual recordings are allowed to contain less than one cycle.
There is no per-log minimum-cycle rule. By default, the search extends to a
period twice as long as the longest recording, so even that longest recording
may contain only half a cycle; shorter logs may contain much less.

The shared period is chosen by minimizing the mean, across logs, of:

    sine_model_SSE / trend_only_SSE

This gives each recording equal influence instead of letting the longest log
dominate simply because it has more samples.

The script also:
  * compares the shared-sine model with a trend-only baseline;
  * reports descriptive AIC/BIC changes on the smoothed data;
  * calculates leave-one-log-out shared-period estimates;
  * previews every fit, the period objective, leave-one-out results, and a
    phase-folded normalized overlay;
  * never saves PNG files;
  * can optionally write CSV summaries.

Dependencies:
    pip install numpy matplotlib
"""

from __future__ import annotations

import argparse
import csv
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import matplotlib.pyplot as plt
import numpy as np


HUB_HZ_DEFAULT = 16_000_000.0
LOCAL_HZ_DEFAULT = 16_000_000.0
SMOOTH_SECONDS_DEFAULT = 10.0
MIN_PERIOD_SECONDS_DEFAULT = 40.0
MAX_PERIOD_FACTOR_DEFAULT = 2.0

NODE_ALIASES = {
    "reward_port": "fairy",
    "fairy": "fairy",
}

RECORD_PATTERN = re.compile(r"([A-Z][A-Z0-9_]*),")


@dataclass(frozen=True)
class PairRecord:
    source_line: int
    hub_ticks: int
    local_ticks: int


@dataclass(frozen=True)
class SyncRecord:
    source_line: int
    hub_ticks: int
    state: str
    slope_ppb: float


@dataclass(frozen=True)
class RateSeries:
    source: str
    time_s: np.ndarray
    rate_ppm: np.ndarray


@dataclass(frozen=True)
class PreparedLog:
    path: Path
    raw: RateSeries
    smooth: RateSeries
    fit_time_s: np.ndarray
    fit_rate_ppm: np.ndarray
    pair_count: int
    sync_count: int
    baseline_sse: float
    baseline_coefficients: np.ndarray
    time_reference_s: float


@dataclass(frozen=True)
class PerLogFit:
    path: Path
    period_s: float
    coefficients: np.ndarray
    time_reference_s: float
    amplitude_ppm: float
    phase_rad: float
    offset_ppm: float
    trend_ppm_per_s: float
    baseline_sse: float
    sine_sse: float
    improvement_fraction: float
    delta_aic: float
    delta_bic: float
    r_squared_smoothed: float
    rmse_smoothed_ppm: float
    r_squared_raw: float
    rmse_raw_ppm: float
    fitted_smoothed_ppm: np.ndarray
    fitted_raw_ppm: np.ndarray


@dataclass(frozen=True)
class SharedFit:
    period_s: float
    objective_ratio: float
    mean_improvement_fraction: float
    median_improvement_fraction: float
    pooled_improvement_fraction: float
    total_baseline_sse: float
    total_sine_sse: float
    delta_aic: float
    delta_bic: float
    per_log: tuple[PerLogFit, ...]
    periods_tested_s: np.ndarray
    objective_tested: np.ndarray


def parse_int(text: str) -> int:
    return int(text.strip(), 0)


def is_int(text: str) -> bool:
    try:
        parse_int(text)
        return True
    except (TypeError, ValueError):
        return False


def extract_record(line: str) -> str | None:
    match = RECORD_PATTERN.search(line)
    if match is None:
        return None
    return line[match.start() :].strip()


def split_record(record: str) -> list[str]:
    return [field.strip() for field in next(csv.reader([record]))]


def normalize_node(node: str) -> str:
    stripped = node.strip()
    return NODE_ALIASES.get(stripped, stripped)


def node_and_base(fields: list[str]) -> tuple[str, int]:
    """
    Old records omit the node:
        PAIR_RAW,1,...
        SYNC,1,...

    New records include it:
        PAIR_RAW,fairy,1,...
        SYNC,reward_port,1,...
    """
    if len(fields) < 2:
        return "fairy", 1

    if is_int(fields[1]):
        return "fairy", 1

    return normalize_node(fields[1]), 2


def parse_log(
    path: Path,
    requested_node: str,
) -> tuple[list[PairRecord], list[SyncRecord]]:
    requested = normalize_node(requested_node)

    pairs: list[PairRecord] = []
    sync_rows: list[SyncRecord] = []

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for source_line, original_line in enumerate(handle, start=1):
            record = extract_record(original_line)
            if record is None:
                continue

            try:
                fields = split_record(record)
            except csv.Error:
                continue

            if not fields or fields[0] not in {"PAIR_RAW", "SYNC"}:
                continue

            node, base = node_and_base(fields)
            if normalize_node(node) != requested:
                continue

            try:
                if fields[0] == "PAIR_RAW":
                    if len(fields) < base + 3:
                        continue

                    pairs.append(
                        PairRecord(
                            source_line=source_line,
                            hub_ticks=parse_int(fields[base + 1]),
                            local_ticks=parse_int(fields[base + 2]),
                        )
                    )
                    continue

                # SYNC layout after the optional node field:
                # pulse, hub, local, status, flags, pending, state, slope_ppb,
                # local_ref, hub_ref, rms_ns, prefit_ns, model_step_ns, age
                if len(fields) < base + 14:
                    continue

                sync_rows.append(
                    SyncRecord(
                        source_line=source_line,
                        hub_ticks=parse_int(fields[base + 1]),
                        state=fields[base + 6].strip(),
                        slope_ppb=float(parse_int(fields[base + 7])),
                    )
                )

            except (IndexError, ValueError):
                continue

    return pairs, sync_rows


def pair_rate_series(
    pairs: list[PairRecord],
    *,
    hub_hz: float,
    local_hz: float,
) -> RateSeries:
    rows = sorted(pairs, key=lambda row: row.source_line)

    times: list[float] = []
    rates: list[float] = []
    previous: PairRecord | None = None

    for current in rows:
        if previous is None:
            previous = current
            continue

        hub_delta = current.hub_ticks - previous.hub_ticks
        local_delta = current.local_ticks - previous.local_ticks

        # Do not calculate an interval across a reboot or counter regression.
        if hub_delta <= 0 or local_delta <= 0:
            previous = current
            continue

        hub_interval_s = hub_delta / hub_hz
        local_interval_s = local_delta / local_hz

        rate_ppm = (local_interval_s / hub_interval_s - 1.0) * 1_000_000.0

        if math.isfinite(rate_ppm):
            times.append(current.hub_ticks / hub_hz)
            rates.append(rate_ppm)

        previous = current

    if not times:
        return RateSeries(
            source="pair-rate",
            time_s=np.asarray([], dtype=np.float64),
            rate_ppm=np.asarray([], dtype=np.float64),
        )

    time = np.asarray(times, dtype=np.float64)
    time -= float(np.min(time))

    return RateSeries(
        source="pair-rate",
        time_s=time,
        rate_ppm=np.asarray(rates, dtype=np.float64),
    )


def sync_slope_series(
    sync_rows: list[SyncRecord],
    *,
    hub_hz: float,
) -> RateSeries:
    selected = [
        row
        for row in sync_rows
        if row.state == "TRACK" and math.isfinite(row.slope_ppb)
    ]

    if not selected:
        return RateSeries(
            source="sync-slope",
            time_s=np.asarray([], dtype=np.float64),
            rate_ppm=np.asarray([], dtype=np.float64),
        )

    selected.sort(key=lambda row: row.source_line)

    time = np.asarray(
        [row.hub_ticks / hub_hz for row in selected],
        dtype=np.float64,
    )
    time -= float(np.min(time))

    # This matches the existing plot:
    # model-estimated clock error = -firmware slope ppm.
    rate = np.asarray(
        [-row.slope_ppb / 1000.0 for row in selected],
        dtype=np.float64,
    )

    finite = np.isfinite(time) & np.isfinite(rate)

    return RateSeries(
        source="sync-slope",
        time_s=time[finite],
        rate_ppm=rate[finite],
    )


def choose_source(
    requested: str,
    pair_series: RateSeries,
    sync_series: RateSeries,
) -> RateSeries:
    if requested == "pair-rate":
        return pair_series

    if requested == "sync-slope":
        return sync_series

    if sync_series.rate_ppm.size >= 20:
        return sync_series

    return pair_series


def rolling_median_by_time(
    series: RateSeries,
    window_s: float,
) -> RateSeries:
    if series.rate_ppm.size == 0 or window_s <= 0.0:
        return series

    order = np.argsort(series.time_s)
    time = series.time_s[order]
    values = series.rate_ppm[order]

    half_window = window_s / 2.0
    smoothed = np.empty_like(values)

    for index, center in enumerate(time):
        left = int(np.searchsorted(time, center - half_window, side="left"))
        right = int(np.searchsorted(time, center + half_window, side="right"))
        smoothed[index] = float(np.median(values[left:right]))

    return RateSeries(
        source=f"{series.source}-median-{window_s:g}s",
        time_s=time,
        rate_ppm=smoothed,
    )


def baseline_design(
    time_s: np.ndarray,
    reference_s: float,
    include_trend: bool,
) -> np.ndarray:
    columns = [np.ones(time_s.size)]

    if include_trend:
        columns.append(time_s - reference_s)

    return np.column_stack(columns)


def sine_design(
    time_s: np.ndarray,
    reference_s: float,
    period_s: float,
    include_trend: bool,
) -> np.ndarray:
    omega = 2.0 * math.pi / period_s

    columns = [np.ones(time_s.size)]

    if include_trend:
        columns.append(time_s - reference_s)

    columns.extend(
        [
            np.sin(omega * time_s),
            np.cos(omega * time_s),
        ]
    )

    return np.column_stack(columns)


def solve_design(
    design: np.ndarray,
    values: np.ndarray,
) -> tuple[np.ndarray, np.ndarray, float]:
    coefficients, _, rank, _ = np.linalg.lstsq(
        design,
        values,
        rcond=None,
    )

    # Extremely short arcs can make columns nearly indistinguishable. A rank
    # failure means that this particular period is not identifiable.
    if rank < design.shape[1]:
        raise ValueError("rank-deficient fit")

    fitted = design @ coefficients
    residual = values - fitted
    sse = float(np.dot(residual, residual))

    return coefficients, fitted, sse


def baseline_fit(
    time_s: np.ndarray,
    values: np.ndarray,
    *,
    include_trend: bool,
) -> tuple[np.ndarray, np.ndarray, float, float]:
    reference = float(np.mean(time_s))
    design = baseline_design(
        time_s,
        reference,
        include_trend,
    )
    coefficients, fitted, sse = solve_design(
        design,
        values,
    )
    return coefficients, fitted, sse, reference


def trend_residual_mask(
    time_s: np.ndarray,
    values: np.ndarray,
    *,
    include_trend: bool,
    mad_limit: float,
) -> np.ndarray:
    finite = np.isfinite(time_s) & np.isfinite(values)

    if not np.any(finite):
        return finite

    if mad_limit <= 0.0:
        return finite

    selected_time = time_s[finite]
    selected_values = values[finite]

    try:
        _, fitted, _, _ = baseline_fit(
            selected_time,
            selected_values,
            include_trend=include_trend,
        )
    except ValueError:
        return finite

    residual = selected_values - fitted
    median = float(np.median(residual))
    mad = float(np.median(np.abs(residual - median)))

    if mad <= 0.0:
        return finite

    robust_sigma = 1.4826 * mad
    selected_mask = np.abs(residual - median) <= mad_limit * robust_sigma

    output = np.zeros(values.size, dtype=bool)
    output[np.flatnonzero(finite)] = selected_mask
    return output


def prepare_log(
    path: Path,
    *,
    requested_node: str,
    source: str,
    hub_hz: float,
    local_hz: float,
    smooth_seconds: float,
    outlier_mad: float,
    include_trend: bool,
) -> PreparedLog:
    pairs, sync_rows = parse_log(path, requested_node)

    pair_series = pair_rate_series(
        pairs,
        hub_hz=hub_hz,
        local_hz=local_hz,
    )
    sync_series = sync_slope_series(
        sync_rows,
        hub_hz=hub_hz,
    )

    raw = choose_source(
        source,
        pair_series,
        sync_series,
    )

    if raw.rate_ppm.size < 8:
        raise ValueError(
            f"too few {raw.source} samples "
            f"(pairs={len(pairs)}, sync={len(sync_rows)}, "
            f"rate_samples={raw.rate_ppm.size})"
        )

    smooth = rolling_median_by_time(
        raw,
        smooth_seconds,
    )

    mask = trend_residual_mask(
        smooth.time_s,
        smooth.rate_ppm,
        include_trend=include_trend,
        mad_limit=outlier_mad,
    )

    fit_time = smooth.time_s[mask]
    fit_rate = smooth.rate_ppm[mask]

    minimum_points = 6 if include_trend else 5
    if fit_time.size < minimum_points:
        raise ValueError(f"too few samples after filtering ({fit_time.size})")

    baseline_coefficients, _, baseline_sse, reference = baseline_fit(
        fit_time,
        fit_rate,
        include_trend=include_trend,
    )

    if not math.isfinite(baseline_sse) or baseline_sse <= 0.0:
        raise ValueError("trend-only baseline has zero variance")

    return PreparedLog(
        path=path,
        raw=raw,
        smooth=smooth,
        fit_time_s=fit_time,
        fit_rate_ppm=fit_rate,
        pair_count=len(pairs),
        sync_count=len(sync_rows),
        baseline_sse=baseline_sse,
        baseline_coefficients=baseline_coefficients,
        time_reference_s=reference,
    )


def solve_log_period(
    log: PreparedLog,
    period_s: float,
    *,
    include_trend: bool,
) -> tuple[np.ndarray, np.ndarray, float]:
    design = sine_design(
        log.fit_time_s,
        log.time_reference_s,
        period_s,
        include_trend,
    )

    return solve_design(
        design,
        log.fit_rate_ppm,
    )


def objective_for_period(
    logs: list[PreparedLog],
    period_s: float,
    *,
    include_trend: bool,
) -> float:
    ratios: list[float] = []

    for log in logs:
        try:
            _, _, sine_sse = solve_log_period(
                log,
                period_s,
                include_trend=include_trend,
            )
        except ValueError:
            return math.inf

        ratios.append(sine_sse / log.baseline_sse)

    return float(np.mean(ratios))


def search_shared_period(
    logs: list[PreparedLog],
    *,
    min_period_s: float,
    max_period_s: float,
    grid_points: int,
    refine_points: int,
    include_trend: bool,
) -> tuple[float, np.ndarray, np.ndarray]:
    if max_period_s <= min_period_s:
        raise ValueError(f"empty period range: {min_period_s:g} to {max_period_s:g} s")

    coarse_periods = np.geomspace(
        min_period_s,
        max_period_s,
        max(grid_points, 200),
    )

    coarse_objective = np.asarray(
        [
            objective_for_period(
                logs,
                float(period),
                include_trend=include_trend,
            )
            for period in coarse_periods
        ],
        dtype=np.float64,
    )

    finite = np.isfinite(coarse_objective)
    if not np.any(finite):
        raise ValueError("all candidate periods were rank-deficient")

    best_index = int(np.nanargmin(coarse_objective))

    left_index = max(0, best_index - 3)
    right_index = min(
        coarse_periods.size - 1,
        best_index + 3,
    )

    refined_periods = np.linspace(
        float(coarse_periods[left_index]),
        float(coarse_periods[right_index]),
        max(refine_points, 200),
    )

    refined_objective = np.asarray(
        [
            objective_for_period(
                logs,
                float(period),
                include_trend=include_trend,
            )
            for period in refined_periods
        ],
        dtype=np.float64,
    )

    refined_finite = np.isfinite(refined_objective)
    if not np.any(refined_finite):
        raise ValueError("all refined candidate periods failed")

    refined_best = int(np.nanargmin(refined_objective))
    best_period = float(refined_periods[refined_best])

    all_periods = np.concatenate([coarse_periods, refined_periods])
    all_objective = np.concatenate([coarse_objective, refined_objective])

    order = np.argsort(all_periods)

    return (
        best_period,
        all_periods[order],
        all_objective[order],
    )


def information_criteria(
    sse: float,
    sample_count: int,
    parameter_count: int,
) -> tuple[float, float]:
    safe_sse = max(
        sse,
        np.finfo(np.float64).tiny,
    )

    aic = sample_count * math.log(safe_sse / sample_count) + 2.0 * parameter_count

    bic = sample_count * math.log(safe_sse / sample_count) + parameter_count * math.log(
        sample_count
    )

    return aic, bic


def goodness(
    observed: np.ndarray,
    fitted: np.ndarray,
) -> tuple[float, float]:
    residual = observed - fitted
    sse = float(np.dot(residual, residual))
    rmse = math.sqrt(sse / max(1, observed.size))

    centered = observed - float(np.mean(observed))
    total = float(np.dot(centered, centered))

    r_squared = 1.0 - sse / total if total > 0.0 else math.nan

    return r_squared, rmse


def evaluate_sine(
    time_s: np.ndarray,
    *,
    coefficients: np.ndarray,
    reference_s: float,
    period_s: float,
    include_trend: bool,
) -> np.ndarray:
    return (
        sine_design(
            time_s,
            reference_s,
            period_s,
            include_trend,
        )
        @ coefficients
    )


def build_shared_fit(
    logs: list[PreparedLog],
    *,
    period_s: float,
    periods_tested_s: np.ndarray,
    objective_tested: np.ndarray,
    include_trend: bool,
) -> SharedFit:
    per_log: list[PerLogFit] = []

    total_baseline_sse = 0.0
    total_sine_sse = 0.0
    improvements: list[float] = []

    baseline_parameter_count = 2 if include_trend else 1
    sine_parameter_count = baseline_parameter_count + 2

    for log in logs:
        coefficients, fitted_smoothed, sine_sse = solve_log_period(
            log,
            period_s,
            include_trend=include_trend,
        )

        fitted_raw = evaluate_sine(
            log.raw.time_s,
            coefficients=coefficients,
            reference_s=log.time_reference_s,
            period_s=period_s,
            include_trend=include_trend,
        )

        if include_trend:
            offset, trend, sine_coefficient, cosine_coefficient = (
                float(value) for value in coefficients
            )
        else:
            offset, sine_coefficient, cosine_coefficient = (
                float(value) for value in coefficients
            )
            trend = 0.0

        amplitude = math.hypot(
            sine_coefficient,
            cosine_coefficient,
        )
        phase = math.atan2(
            cosine_coefficient,
            sine_coefficient,
        )

        improvement = 1.0 - sine_sse / log.baseline_sse

        smoothed_r2, smoothed_rmse = goodness(
            log.fit_rate_ppm,
            fitted_smoothed,
        )
        raw_r2, raw_rmse = goodness(
            log.raw.rate_ppm,
            fitted_raw,
        )

        baseline_aic, baseline_bic = information_criteria(
            log.baseline_sse,
            log.fit_rate_ppm.size,
            baseline_parameter_count,
        )
        sine_aic, sine_bic = information_criteria(
            sine_sse,
            log.fit_rate_ppm.size,
            sine_parameter_count,
        )

        per_log.append(
            PerLogFit(
                path=log.path,
                period_s=period_s,
                coefficients=coefficients,
                time_reference_s=log.time_reference_s,
                amplitude_ppm=amplitude,
                phase_rad=phase,
                offset_ppm=offset,
                trend_ppm_per_s=trend,
                baseline_sse=log.baseline_sse,
                sine_sse=sine_sse,
                improvement_fraction=improvement,
                delta_aic=sine_aic - baseline_aic,
                delta_bic=sine_bic - baseline_bic,
                r_squared_smoothed=smoothed_r2,
                rmse_smoothed_ppm=smoothed_rmse,
                r_squared_raw=raw_r2,
                rmse_raw_ppm=raw_rmse,
                fitted_smoothed_ppm=fitted_smoothed,
                fitted_raw_ppm=fitted_raw,
            )
        )

        total_baseline_sse += log.baseline_sse
        total_sine_sse += sine_sse
        improvements.append(improvement)

    objective_ratio = float(
        np.mean([fit.sine_sse / fit.baseline_sse for fit in per_log])
    )

    total_samples = sum(log.fit_rate_ppm.size for log in logs)
    log_count = len(logs)

    # The shared period adds one global parameter.
    baseline_parameter_total = log_count * baseline_parameter_count
    sine_parameter_total = log_count * sine_parameter_count + 1

    baseline_aic, baseline_bic = information_criteria(
        total_baseline_sse,
        total_samples,
        baseline_parameter_total,
    )
    sine_aic, sine_bic = information_criteria(
        total_sine_sse,
        total_samples,
        sine_parameter_total,
    )

    return SharedFit(
        period_s=period_s,
        objective_ratio=objective_ratio,
        mean_improvement_fraction=float(np.mean(improvements)),
        median_improvement_fraction=float(np.median(improvements)),
        pooled_improvement_fraction=(1.0 - total_sine_sse / total_baseline_sse),
        total_baseline_sse=total_baseline_sse,
        total_sine_sse=total_sine_sse,
        delta_aic=sine_aic - baseline_aic,
        delta_bic=sine_bic - baseline_bic,
        per_log=tuple(per_log),
        periods_tested_s=periods_tested_s,
        objective_tested=objective_tested,
    )


def leave_one_out_periods(
    logs: list[PreparedLog],
    *,
    min_period_s: float,
    max_period_s: float,
    grid_points: int,
    refine_points: int,
    include_trend: bool,
) -> list[tuple[str, float]]:
    if len(logs) < 3:
        return []

    output: list[tuple[str, float]] = []

    for omitted_index, omitted in enumerate(logs):
        training = [log for index, log in enumerate(logs) if index != omitted_index]

        period, _, _ = search_shared_period(
            training,
            min_period_s=min_period_s,
            max_period_s=max_period_s,
            grid_points=max(400, grid_points // 2),
            refine_points=max(300, refine_points // 2),
            include_trend=include_trend,
        )

        output.append((omitted.path.name, period))

    return output


def discover_logs(
    folder: Path,
    recursive: bool,
) -> list[Path]:
    iterator: Iterable[Path] = (
        folder.rglob("*.log") if recursive else folder.glob("*.log")
    )

    return sorted(path for path in iterator if path.is_file())


def print_shared_summary(
    logs: list[PreparedLog],
    fit: SharedFit,
    loo: list[tuple[str, float]],
    *,
    min_period_s: float,
    max_period_s: float,
) -> None:
    print()
    print("=" * 104)
    print("Shared-period result")
    print("=" * 104)
    print(f"usable logs                         : {len(logs)}")
    print(
        f"searched period range              : {min_period_s:.6f} to {max_period_s:.6f} s"
    )
    print(f"best shared period                 : {fit.period_s:.6f} s")
    print(f"mean normalized SSE ratio          : {fit.objective_ratio:.6f}")
    print(
        f"mean per-log baseline improvement  : {100.0 * fit.mean_improvement_fraction:.3f} %"
    )
    print(
        f"median per-log improvement         : {100.0 * fit.median_improvement_fraction:.3f} %"
    )
    print(
        f"pooled baseline improvement        : {100.0 * fit.pooled_improvement_fraction:.3f} %"
    )
    print(f"descriptive delta AIC (sine-base)  : {fit.delta_aic:.3f}")
    print(f"descriptive delta BIC (sine-base)  : {fit.delta_bic:.3f}")

    boundary_tolerance = 0.01
    if fit.period_s <= min_period_s * (1.0 + boundary_tolerance):
        print("WARNING: best period is at the lower search boundary.")
    if fit.period_s >= max_period_s * (1.0 - boundary_tolerance):
        print(
            "WARNING: best period is at the upper search boundary; increase --max-period."
        )

    print()
    print(
        f"{'log':38s} {'duration':>9s} {'cycles':>8s} {'amp ppm':>9s} "
        f"{'improve':>9s} {'R2 smooth':>10s} {'R2 raw':>8s} "
        f"{'dAIC':>9s} {'dBIC':>9s}"
    )

    fit_by_name = {item.path: item for item in fit.per_log}

    for log in logs:
        item = fit_by_name[log.path]
        duration = float(np.max(log.fit_time_s) - np.min(log.fit_time_s))
        cycles = duration / fit.period_s

        print(
            f"{log.path.name:38s} "
            f"{duration:9.2f} "
            f"{cycles:8.3f} "
            f"{item.amplitude_ppm:9.2f} "
            f"{100.0 * item.improvement_fraction:8.2f}% "
            f"{item.r_squared_smoothed:10.3f} "
            f"{item.r_squared_raw:8.3f} "
            f"{item.delta_aic:9.2f} "
            f"{item.delta_bic:9.2f}"
        )

    if loo:
        periods = np.asarray(
            [period for _, period in loo],
            dtype=np.float64,
        )

        print()
        print("=" * 104)
        print("Leave-one-log-out shared periods")
        print("=" * 104)

        for omitted, period in loo:
            print(f"omit {omitted:43s} -> {period:.6f} s")

        print(
            f"LOO median                          : {float(np.median(periods)):.6f} s"
        )
        print(f"LOO minimum                         : {float(np.min(periods)):.6f} s")
        print(f"LOO maximum                         : {float(np.max(periods)):.6f} s")
        if periods.size > 1:
            print(
                f"LOO standard deviation              : {float(np.std(periods, ddof=1)):.6f} s"
            )


def write_fit_summary(
    path: Path,
    logs: list[PreparedLog],
    fit: SharedFit,
) -> None:
    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    fit_by_path = {item.path: item for item in fit.per_log}

    with path.open(
        "w",
        newline="",
        encoding="utf-8",
    ) as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "file",
                "source",
                "raw_sample_count",
                "fit_sample_count",
                "duration_s",
                "shared_period_s",
                "observed_cycles",
                "amplitude_ppm",
                "phase_rad",
                "offset_ppm",
                "trend_ppm_per_s",
                "baseline_sse",
                "sine_sse",
                "improvement_fraction",
                "r_squared_smoothed",
                "rmse_smoothed_ppm",
                "r_squared_raw",
                "rmse_raw_ppm",
                "delta_aic",
                "delta_bic",
            ],
        )
        writer.writeheader()

        for log in logs:
            item = fit_by_path[log.path]
            duration = float(np.max(log.fit_time_s) - np.min(log.fit_time_s))

            writer.writerow(
                {
                    "file": str(log.path),
                    "source": log.raw.source,
                    "raw_sample_count": log.raw.rate_ppm.size,
                    "fit_sample_count": log.fit_rate_ppm.size,
                    "duration_s": duration,
                    "shared_period_s": fit.period_s,
                    "observed_cycles": duration / fit.period_s,
                    "amplitude_ppm": item.amplitude_ppm,
                    "phase_rad": item.phase_rad,
                    "offset_ppm": item.offset_ppm,
                    "trend_ppm_per_s": item.trend_ppm_per_s,
                    "baseline_sse": item.baseline_sse,
                    "sine_sse": item.sine_sse,
                    "improvement_fraction": item.improvement_fraction,
                    "r_squared_smoothed": item.r_squared_smoothed,
                    "rmse_smoothed_ppm": item.rmse_smoothed_ppm,
                    "r_squared_raw": item.r_squared_raw,
                    "rmse_raw_ppm": item.rmse_raw_ppm,
                    "delta_aic": item.delta_aic,
                    "delta_bic": item.delta_bic,
                }
            )


def write_loo_summary(
    path: Path,
    loo: list[tuple[str, float]],
) -> None:
    path.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    with path.open(
        "w",
        newline="",
        encoding="utf-8",
    ) as handle:
        writer = csv.writer(handle)
        writer.writerow(["omitted_log", "shared_period_s"])
        writer.writerows(loo)


def preview_log_fit(
    log: PreparedLog,
    item: PerLogFit,
    shared_period_s: float,
    *,
    include_trend: bool,
) -> None:
    figure, axis = plt.subplots(figsize=(12, 5))

    axis.scatter(
        log.raw.time_s,
        log.raw.rate_ppm,
        s=9,
        alpha=0.25,
        label=f"Raw {log.raw.source}",
    )

    axis.plot(
        log.smooth.time_s,
        log.smooth.rate_ppm,
        linewidth=1.4,
        alpha=0.9,
        label=f"Smoothed ({log.smooth.source})",
    )

    dense_time = np.linspace(
        float(np.min(log.smooth.time_s)),
        float(np.max(log.smooth.time_s)),
        2000,
    )

    dense_fit = evaluate_sine(
        dense_time,
        coefficients=item.coefficients,
        reference_s=item.time_reference_s,
        period_s=shared_period_s,
        include_trend=include_trend,
    )

    axis.plot(
        dense_time,
        dense_fit,
        linewidth=2.2,
        label=(
            f"Shared-period fit: T={shared_period_s:.2f} s, "
            f"A={item.amplitude_ppm:.1f} ppm"
        ),
    )

    duration = float(np.max(log.fit_time_s) - np.min(log.fit_time_s))
    cycles = duration / shared_period_s

    axis.set_title(
        f"{log.path.name}: shared Fairy period\n"
        f"observed cycles={cycles:.3f}, "
        f"trend-baseline improvement={100.0 * item.improvement_fraction:.1f}%, "
        f"smoothed R²={item.r_squared_smoothed:.3f}"
    )
    axis.set_xlabel("Elapsed time within recording (s)")
    axis.set_ylabel("Fairy clock-rate error (ppm)")
    axis.grid(True, alpha=0.3)
    axis.legend()
    figure.tight_layout()


def preview_objective(
    fit: SharedFit,
) -> None:
    finite = np.isfinite(fit.periods_tested_s) & np.isfinite(fit.objective_tested)

    periods = fit.periods_tested_s[finite]
    objective = fit.objective_tested[finite]

    figure, axis = plt.subplots(figsize=(10, 5))
    axis.plot(
        periods,
        objective,
        linewidth=1.4,
    )
    axis.axvline(
        fit.period_s,
        linestyle="--",
        linewidth=1.5,
        label=f"Best shared period: {fit.period_s:.2f} s",
    )
    axis.set_xscale("log")
    axis.set_title("Shared-period search objective")
    axis.set_xlabel("Candidate shared period (s)")
    axis.set_ylabel("Mean sine SSE / trend-only SSE")
    axis.grid(True, alpha=0.3)
    axis.legend()
    figure.tight_layout()


def preview_leave_one_out(
    loo: list[tuple[str, float]],
    shared_period_s: float,
) -> None:
    if not loo:
        return

    labels = [name for name, _ in loo]
    periods = np.asarray(
        [period for _, period in loo],
        dtype=np.float64,
    )
    x = np.arange(len(loo))

    figure, axis = plt.subplots(figsize=(max(10, 0.8 * len(loo)), 5))
    axis.scatter(
        x,
        periods,
        s=45,
        label="Leave-one-out period",
    )
    axis.axhline(
        shared_period_s,
        linestyle="--",
        linewidth=1.5,
        label=f"All logs: {shared_period_s:.2f} s",
    )
    axis.set_xticks(x)
    axis.set_xticklabels(
        labels,
        rotation=60,
        ha="right",
    )
    axis.set_title("Sensitivity of the shared period to each recording")
    axis.set_xlabel("Omitted recording")
    axis.set_ylabel("Shared period from remaining logs (s)")
    axis.grid(True, alpha=0.3)
    axis.legend()
    figure.tight_layout()


def preview_phase_folded(
    logs: list[PreparedLog],
    fit: SharedFit,
    *,
    include_trend: bool,
) -> None:
    figure, axis = plt.subplots(figsize=(10, 5))

    fit_by_path = {item.path: item for item in fit.per_log}

    for log in logs:
        item = fit_by_path[log.path]

        if item.amplitude_ppm <= 0.0:
            continue

        time = log.fit_time_s
        omega = 2.0 * math.pi / fit.period_s

        if include_trend:
            baseline = item.offset_ppm + item.trend_ppm_per_s * (
                time - item.time_reference_s
            )
        else:
            baseline = np.full_like(
                time,
                item.offset_ppm,
            )

        normalized = (log.fit_rate_ppm - baseline) / item.amplitude_ppm

        # A*sin(wt+phase). Shift phase so all fitted curves align to sin(theta).
        phase = np.mod(
            omega * time + item.phase_rad,
            2.0 * math.pi,
        )

        axis.scatter(
            phase,
            normalized,
            s=9,
            alpha=0.28,
            label=log.path.name,
        )

    phase_grid = np.linspace(
        0.0,
        2.0 * math.pi,
        1000,
    )
    axis.plot(
        phase_grid,
        np.sin(phase_grid),
        linewidth=2.3,
        label="Shared sinusoidal shape",
    )

    axis.set_title("Phase-folded Fairy clock-rate variation at the shared period")
    axis.set_xlabel("Fitted phase within cycle (rad)")
    axis.set_ylabel("Detrended rate / fitted amplitude")
    axis.grid(True, alpha=0.3)
    axis.legend(fontsize="small")
    figure.tight_layout()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Fit one shared sinusoidal period to Fairy clock-rate variation "
            "across all .log files in a folder."
        )
    )

    parser.add_argument(
        "folder",
        type=Path,
        help="Folder containing .log files",
    )
    parser.add_argument(
        "--recursive",
        action="store_true",
        help="Search subdirectories",
    )
    parser.add_argument(
        "--node",
        default="fairy",
        help="Node to analyse; reward_port is treated as fairy",
    )
    parser.add_argument(
        "--source",
        choices=("auto", "sync-slope", "pair-rate"),
        default="auto",
        help="auto prefers TRACK slope and falls back to adjacent PAIR_RAW rate",
    )
    parser.add_argument(
        "--hub-hz",
        type=float,
        default=HUB_HZ_DEFAULT,
    )
    parser.add_argument(
        "--local-hz",
        type=float,
        default=LOCAL_HZ_DEFAULT,
    )
    parser.add_argument(
        "--smooth-seconds",
        type=float,
        default=SMOOTH_SECONDS_DEFAULT,
        help="Width of each log's time-domain rolling median",
    )
    parser.add_argument(
        "--min-period",
        type=float,
        default=MIN_PERIOD_SECONDS_DEFAULT,
        help="Minimum shared period in seconds",
    )
    parser.add_argument(
        "--max-period",
        type=float,
        default=None,
        help=(
            "Maximum shared period. When omitted, it is "
            "--max-period-factor times the longest recording."
        ),
    )
    parser.add_argument(
        "--max-period-factor",
        type=float,
        default=MAX_PERIOD_FACTOR_DEFAULT,
        help=(
            "Automatic maximum period relative to the longest recording. "
            "Default 2 permits only half a cycle in the longest log and fewer "
            "cycles in shorter logs."
        ),
    )
    parser.add_argument(
        "--grid-points",
        type=int,
        default=2400,
    )
    parser.add_argument(
        "--refine-points",
        type=int,
        default=1400,
    )
    parser.add_argument(
        "--outlier-mad",
        type=float,
        default=6.0,
        help="MAD limit around each log's trend; 0 disables filtering",
    )
    parser.add_argument(
        "--no-linear-trend",
        action="store_true",
        help="Give each log an offset but no linear trend",
    )
    parser.add_argument(
        "--no-loo",
        action="store_true",
        help="Skip leave-one-log-out period sensitivity analysis",
    )
    parser.add_argument(
        "--summary-csv",
        type=Path,
        default=None,
        help="Optional per-log fit summary CSV",
    )
    parser.add_argument(
        "--loo-csv",
        type=Path,
        default=None,
        help="Optional leave-one-out summary CSV",
    )
    parser.add_argument(
        "--no-preview",
        action="store_true",
        help="Print/write results without opening Matplotlib",
    )

    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if not args.folder.is_dir():
        raise SystemExit(f"Folder not found: {args.folder}")

    if args.hub_hz <= 0.0 or args.local_hz <= 0.0:
        raise SystemExit("Clock frequencies must be positive")

    if args.smooth_seconds < 0.0:
        raise SystemExit("--smooth-seconds cannot be negative")

    if args.min_period <= 0.0:
        raise SystemExit("--min-period must be positive")

    if args.max_period_factor <= 0.0:
        raise SystemExit("--max-period-factor must be positive")

    logs_found = discover_logs(
        args.folder,
        args.recursive,
    )

    if not logs_found:
        raise SystemExit(f"No .log files found in {args.folder}")

    include_trend = not args.no_linear_trend

    prepared: list[PreparedLog] = []
    failures: list[tuple[Path, str]] = []

    print(f"Found {len(logs_found)} log file(s).")
    print(
        "Preparing independent recordings. No per-log minimum-cycle "
        "requirement is applied."
    )
    print()

    for path in logs_found:
        try:
            log = prepare_log(
                path,
                requested_node=args.node,
                source=args.source,
                hub_hz=args.hub_hz,
                local_hz=args.local_hz,
                smooth_seconds=args.smooth_seconds,
                outlier_mad=args.outlier_mad,
                include_trend=include_trend,
            )
        except ValueError as error:
            failures.append((path, str(error)))
            continue

        prepared.append(log)

        duration = float(np.max(log.fit_time_s) - np.min(log.fit_time_s))

        print(
            f"{path.name:42s} "
            f"source={log.raw.source:10s} "
            f"raw={log.raw.rate_ppm.size:5d} "
            f"fit={log.fit_rate_ppm.size:5d} "
            f"duration={duration:9.3f} s"
        )

    if not prepared:
        for path, reason in failures:
            print(f"SKIP {path.name}: {reason}")
        raise SystemExit("No usable log remained.")

    longest_duration = max(
        float(np.max(log.fit_time_s) - np.min(log.fit_time_s)) for log in prepared
    )

    max_period = (
        args.max_period
        if args.max_period is not None
        else longest_duration * args.max_period_factor
    )

    if max_period <= args.min_period:
        raise SystemExit(
            f"Maximum period {max_period:g}s is not above "
            f"minimum period {args.min_period:g}s"
        )

    print()
    print(
        f"Searching one shared period from {args.min_period:.3f} to {max_period:.3f} s."
    )
    print(
        "Short logs may contain substantially less than one cycle; "
        "their phase/amplitude are fitted independently."
    )

    best_period, tested_periods, tested_objective = search_shared_period(
        prepared,
        min_period_s=args.min_period,
        max_period_s=max_period,
        grid_points=args.grid_points,
        refine_points=args.refine_points,
        include_trend=include_trend,
    )

    shared_fit = build_shared_fit(
        prepared,
        period_s=best_period,
        periods_tested_s=tested_periods,
        objective_tested=tested_objective,
        include_trend=include_trend,
    )

    loo: list[tuple[str, float]] = []
    if not args.no_loo:
        loo = leave_one_out_periods(
            prepared,
            min_period_s=args.min_period,
            max_period_s=max_period,
            grid_points=args.grid_points,
            refine_points=args.refine_points,
            include_trend=include_trend,
        )

    print_shared_summary(
        prepared,
        shared_fit,
        loo,
        min_period_s=args.min_period,
        max_period_s=max_period,
    )

    if failures:
        print()
        print("=" * 104)
        print("Skipped logs")
        print("=" * 104)
        for path, reason in failures:
            print(f"{path.name}: {reason}")

    if args.summary_csv is not None:
        write_fit_summary(
            args.summary_csv,
            prepared,
            shared_fit,
        )
        print()
        print(f"Fit summary written to: {args.summary_csv}")

    if args.loo_csv is not None:
        write_loo_summary(
            args.loo_csv,
            loo,
        )
        print(f"Leave-one-out summary written to: {args.loo_csv}")

    if not args.no_preview:
        fit_by_path = {item.path: item for item in shared_fit.per_log}

        for log in prepared:
            preview_log_fit(
                log,
                fit_by_path[log.path],
                shared_fit.period_s,
                include_trend=include_trend,
            )

        preview_objective(shared_fit)
        preview_leave_one_out(
            loo,
            shared_fit.period_s,
        )
        preview_phase_folded(
            prepared,
            shared_fit,
            include_trend=include_trend,
        )
        plt.show()


if __name__ == "__main__":
    main()
