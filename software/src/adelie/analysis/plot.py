from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from itertools import cycle
import math
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patheffects as path_effects
import numpy as np
from cycler import cycler
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.ticker import ScalarFormatter


# Poster palette shared with the established Adelie analysis plotter. Colours
# remain soft on screen while retaining enough contrast for printed posters.
PALETTE = {
    "ink": "#302A43",
    "muted": "#70677F",
    "paper": "#FCF9FF",
    "panel": "#FFFFFF",
    "grid": "#EAE3F2",
    "lavender": "#8D79C6",
    "violet": "#B095D8",
    "lilac": "#D2BDEA",
    "orchid": "#DFA6CF",
    "rose": "#E9B7C9",
    "mint": "#83C8BD",
    "sky": "#94BFE4",
    "amber": "#E6C681",
    "reset": "#C9689C",
}

COLORS = [
    PALETTE["lavender"],
    PALETTE["mint"],
    PALETTE["orchid"],
    PALETTE["sky"],
    PALETTE["amber"],
    PALETTE["violet"],
]

POSTER_CMAP = LinearSegmentedColormap.from_list(
    "adelie_pastel",
    [PALETTE["paper"], PALETTE["lilac"], PALETTE["lavender"], PALETTE["ink"]],
)


def _read(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def _number(row: dict[str, str], key: str) -> float | None:
    value = row.get(key)
    if value in (None, ""):
        return None
    try:
        number = float(value)
    except ValueError:
        return None
    return number if math.isfinite(number) else None


def _node(row: dict[str, str], prefix: str = "source") -> str:
    name = row.get(f"{prefix}_node")
    if name:
        return name
    address = row.get(f"{prefix}_address") or row.get(prefix)
    if not address:
        return "unknown"
    try:
        return f"node 0x{int(address):02x}"
    except ValueError:
        return "unknown"


def _style() -> None:
    plt.rcParams.update(
        {
            "figure.facecolor": PALETTE["paper"],
            "savefig.facecolor": PALETTE["paper"],
            "axes.facecolor": PALETTE["panel"],
            "axes.edgecolor": PALETTE["grid"],
            "axes.labelcolor": PALETTE["ink"],
            "axes.titlecolor": PALETTE["ink"],
            "axes.titleweight": "bold",
            "axes.titlesize": 21.0,
            "axes.titlelocation": "left",
            "axes.labelsize": 15.5,
            "axes.prop_cycle": cycler(color=COLORS),
            "text.color": PALETTE["ink"],
            "xtick.color": PALETTE["muted"],
            "ytick.color": PALETTE["muted"],
            "xtick.labelsize": 13.0,
            "ytick.labelsize": 13.0,
            "font.family": "sans-serif",
            "font.sans-serif": ["Aptos", "Avenir Next", "Inter", "DejaVu Sans"],
            "font.size": 11.5,
            "lines.linewidth": 2.2,
            "lines.solid_capstyle": "round",
            "lines.solid_joinstyle": "round",
            "patch.edgecolor": "none",
            "legend.fontsize": 10.5,
            "legend.frameon": True,
            "legend.fancybox": True,
            "legend.framealpha": 0.94,
            "legend.facecolor": "#FFFFFFE8",
            "legend.edgecolor": PALETTE["grid"],
            "grid.color": PALETTE["grid"],
            "grid.linewidth": 0.9,
            "grid.alpha": 0.9,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.spines.left": False,
            "axes.spines.bottom": False,
            "figure.dpi": 120,
            "savefig.dpi": 320,
            "savefig.bbox": "tight",
            "savefig.pad_inches": 0.16,
        }
    )


def _style_axis(axis: plt.Axes) -> None:
    axis.set_axisbelow(True)
    if axis.images:
        axis.grid(False)
    else:
        axis.grid(True, axis="y")
        axis.grid(False, axis="x")
    # Enforce poster-readable sizing on every axis, including axes whose
    # tick labels were assigned explicitly elsewhere in the plotter.
    axis.tick_params(axis="both", which="both",
                     length=0, pad=7, labelsize=13.0)
    axis.xaxis.label.set_size(15.5)
    axis.yaxis.label.set_size(15.5)
    axis.title.set_size(21.0)
    axis.margins(x=0.015)
    axis.title.set_path_effects(
        [path_effects.withStroke(linewidth=2.0, foreground=PALETTE["paper"])]
    )
    legend = axis.get_legend()
    if legend is not None:
        legend.set_frame_on(True)
        legend.get_frame().set_facecolor(PALETTE["panel"])
        legend.get_frame().set_edgecolor(PALETTE["grid"])
        legend.get_frame().set_linewidth(0.8)
        legend.get_frame().set_alpha(0.94)
        legend.get_frame().set_boxstyle("round,pad=0.3,rounding_size=0.2")


def _plain(axis: plt.Axes, which: str = "y") -> None:
    formatter = ScalarFormatter(useOffset=False)
    formatter.set_scientific(False)
    if which == "y":
        axis.yaxis.set_major_formatter(formatter)
    else:
        axis.xaxis.set_major_formatter(formatter)


def _save(fig: plt.Figure, output_directory: Path, name: str, outputs: list[Path]) -> None:
    for axis in fig.axes:
        _style_axis(axis)
    path = output_directory / name
    fig.savefig(path)
    fig.savefig(path.with_suffix(".svg"))
    plt.close(fig)
    outputs.append(path)


def _elapsed(rows: list[dict[str, str]], key: str) -> tuple[list[float], int]:
    values = [int(float(row[key])) for row in rows if row.get(key)]
    if not values:
        return [], 0
    origin = min(values)
    return [
        (int(float(row[key])) - origin) / 1e9 if row.get(key) else math.nan
        for row in rows
    ], origin


def _time_units(values_us: list[float]) -> tuple[float, str]:
    finite = np.abs(np.asarray(
        [value for value in values_us if math.isfinite(value)]))
    if len(finite) and float(np.percentile(finite, 95)) >= 1_000:
        return 1_000.0, "ms"
    return 1.0, "µs"


def _annotate_summary(
    axis: plt.Axes,
    values: list[float],
    unit: str,
    *,
    signed: bool = False,
    x: float = 0.98,
    y: float = 0.96,
) -> None:
    finite = np.asarray([value for value in values if math.isfinite(value)])
    if not len(finite):
        return
    percentile_values = np.abs(finite) if signed else finite
    p95 = float(np.percentile(percentile_values, 95))
    maximum = float(np.max(percentile_values))
    p95_label = "|P95|" if signed else "P95"
    maximum_label = "|Max|" if signed else "Max"
    axis.text(
        x,
        y,
        f"n={len(finite):,}  •  Median {np.median(finite):.3g} {unit}  •  "
        f"{p95_label} {p95:.3g} {unit}  •  {maximum_label} {maximum:.3g} {unit}",
        transform=axis.transAxes,
        ha="right" if x >= 0.5 else "left",
        va="top",
        color=PALETTE["ink"],
        fontsize=10.2,
        fontweight="semibold",
        bbox={
            "boxstyle": "round,pad=0.42,rounding_size=0.2",
            "facecolor": "#F2EAFB",
            "edgecolor": "none",
            "alpha": 0.96,
        },
        zorder=10,
    )


def _poster_boxplot(
    axis: plt.Axes,
    groups: list[list[float]],
    labels: list[str],
) -> None:
    axis.boxplot(
        groups,
        tick_labels=labels,
        showfliers=True,
        patch_artist=True,
        boxprops={
            "facecolor": PALETTE["lilac"],
            "edgecolor": PALETTE["lavender"],
            "linewidth": 1.3,
            "alpha": 0.74,
        },
        whiskerprops={"color": PALETTE["lavender"], "linewidth": 1.2},
        capprops={"color": PALETTE["lavender"], "linewidth": 1.2},
        medianprops={"color": PALETTE["reset"], "linewidth": 2.0},
        flierprops={
            "marker": "o",
            "markerfacecolor": PALETTE["orchid"],
            "markeredgecolor": "none",
            "markersize": 4,
            "alpha": 0.55,
        },
    )


def _plot_sync(quality: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    if not quality:
        return
    origin = min(int(row["received_monotonic_ns"]) for row in quality)
    nodes = sorted({_node(row) for row in quality})

    fig, axes = plt.subplots(2, 1, figsize=(
        11, 8), sharex=True, constrained_layout=True)
    all_rms: list[float] = []
    all_skew: list[float] = []
    for color, node in zip(cycle(COLORS), nodes):
        rows = [row for row in quality if _node(row) == node]
        x = [(int(row["received_monotonic_ns"]) - origin) / 1e9 for row in rows]
        rms = [(_number(row, "rms_ns") or math.nan) / 1_000 for row in rows]
        skew = [(_number(row, "skew_ppb") or math.nan) / 1_000 for row in rows]
        all_rms.extend(rms)
        all_skew.extend(skew)
        axes[0].plot(x, rms, color=color, linewidth=1.6, label=node)
        axes[1].plot(x, skew, color=color, linewidth=1.3, label=node)
    axes[0].set(title="Synchronization residual", ylabel="Model RMS (µs)")
    axes[1].set(title="Estimated clock rate error",
                xlabel="Elapsed time (s)", ylabel="Skew (ppm)")
    axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
    _annotate_summary(axes[0], all_rms, "µs")
    _annotate_summary(axes[1], all_skew, "ppm", signed=True)
    for axis in axes:
        _plain(axis)
    _save(fig, output, "sync_quality_and_skew.png", outputs)

    fig, axes = plt.subplots(2, 1, figsize=(
        11, 7), sharex=True, constrained_layout=True)
    for color, node in zip(cycle(COLORS), nodes):
        rows = [row for row in quality if _node(row) == node]
        x = [(int(row["received_monotonic_ns"]) - origin) / 1e9 for row in rows]
        points = [_number(row, "model_points") or math.nan for row in rows]
        tracking = [1 if int(float(row.get("flags") or 0))
                    & 0x04 else 0 for row in rows]
        axes[0].plot(x, points, color=color, linewidth=1.5, label=node)
        axes[1].step(x, tracking, where="post",
                     color=color, linewidth=1.5, label=node)
    axes[0].set(title="Synchronization model population", ylabel="Points")
    axes[1].set(title="Synchronization tracking state",
                xlabel="Elapsed time (s)", ylabel="Tracking")
    axes[1].set_yticks([0, 1], ["no", "yes"])
    axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
    _save(fig, output, "sync_model_state.png", outputs)


def _plot_predictions(rows: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    if not rows:
        return
    nodes = sorted({_node(row) for row in rows})
    fig, axes = plt.subplots(2, 1, figsize=(11, 8), constrained_layout=True)
    all_error: list[float] = []
    all_rms: list[float] = []
    for color, node in zip(cycle(COLORS), nodes):
        selected = [row for row in rows if _node(row) == node]
        index = [int(float(row["index"])) for row in selected]
        error = [float(row["prediction_error_ns"]) / 1_000 for row in selected]
        rms = [float(row["rolling_rms_ns"]) / 1_000 for row in selected]
        all_error.extend(error)
        all_rms.extend(rms)
        axes[0].plot(index, error, color=color, linewidth=1.1, label=node)
        axes[1].plot(index, rms, color=color, linewidth=1.3, label=node)
    axes[0].axhline(0, color=PALETTE["muted"], linewidth=0.9, alpha=0.8)
    axes[0].set(title="One step synchronization prediction error",
                ylabel="Error (µs)")
    axes[1].set(title="Rolling synchronization fit residual",
                xlabel="Pair index", ylabel="RMS (µs)")
    axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
    _annotate_summary(axes[0], all_error, "µs", signed=True)
    _annotate_summary(axes[1], all_rms, "µs")
    for axis in axes:
        _plain(axis)
    _save(fig, output, "sync_prediction_timeline.png", outputs)

    fig, ax = plt.subplots(figsize=(9, 5.5), constrained_layout=True)
    for color, node in zip(cycle(COLORS), nodes):
        values = [
            float(row["prediction_error_ns"]) / 1_000
            for row in rows
            if _node(row) == node
        ]
        ax.hist(values, bins=45, color=color, alpha=0.48,
                edgecolor=PALETTE["panel"], label=node)
    ax.set(title="One step prediction error distribution",
           xlabel="Error (µs)", ylabel="Count")
    ax.legend(frameon=False)
    _annotate_summary(ax, all_error, "µs", signed=True)
    _plain(ax, "x")
    _save(fig, output, "sync_prediction_distribution.png", outputs)


def _plot_sync_pairs(rows: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    timed = [row for row in rows if row.get("received_monotonic_ns")]
    if not timed:
        return
    origin = min(int(row["received_monotonic_ns"]) for row in timed)
    nodes = sorted({_node(row) for row in timed})
    fig, axes = plt.subplots(2, 1, figsize=(
        11, 8), sharex=True, constrained_layout=True)
    have_interval = False
    have_duration = False
    all_interval_ppm: list[float] = []
    all_duration_us: list[float] = []
    for color, node in zip(cycle(COLORS), nodes):
        selected = sorted(
            [row for row in timed if _node(row) == node],
            key=lambda row: int(row["received_monotonic_ns"]),
        )
        interval_x: list[float] = []
        interval_ppm: list[float] = []
        previous: tuple[float, float] | None = None
        for row in selected:
            local = _number(row, "actual_ticks")
            reference = _number(row, "requested_ticks")
            if local is not None and reference is not None and previous is not None:
                local_delta = local - previous[0]
                reference_delta = reference - previous[1]
                if local_delta > 0 and reference_delta > 0:
                    interval_x.append(
                        (int(row["received_monotonic_ns"]) - origin) / 1e9)
                    interval_ppm.append(
                        (local_delta / reference_delta - 1.0) * 1e6)
            if local is not None and reference is not None:
                previous = (local, reference)
        if interval_ppm:
            have_interval = True
            all_interval_ppm.extend(interval_ppm)
            axes[0].plot(interval_x, interval_ppm, color=color,
                         linewidth=1.1, label=node)

        duration_rows = [row for row in selected if row.get("duration_us")]
        if duration_rows:
            have_duration = True
            durations = [float(row["duration_us"]) for row in duration_rows]
            all_duration_us.extend(durations)
            axes[1].plot(
                [(int(row["received_monotonic_ns"]) - origin) /
                 1e9 for row in duration_rows],
                durations,
                color=color,
                linewidth=1.1,
                label=node,
            )
    if not have_interval and not have_duration:
        plt.close(fig)
        return
    axes[0].axhline(0, color=PALETTE["muted"], linewidth=0.9, alpha=0.8)
    axes[0].set(title="Raw pair interval rate error",
                ylabel="Interval error (ppm)")
    axes[1].set(title="Synchronization observation duration",
                xlabel="Elapsed time (s)", ylabel="Duration (µs)")
    axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
    if all_interval_ppm:
        _annotate_summary(axes[0], all_interval_ppm, "ppm", signed=True)
    if all_duration_us:
        _annotate_summary(axes[1], all_duration_us, "µs")
    for axis in axes:
        _plain(axis)
    _save(fig, output, "sync_pair_detail.png", outputs)


def _plot_command_rtt(
    rows: list[dict[str, str]], output: Path, outputs: list[Path], *, title: str, name: str
) -> None:
    rows = [row for row in rows if row.get(
        "transport_rtt_us") and row.get("sent_monotonic_ns")]
    if not rows:
        return
    raw = [float(row["transport_rtt_us"]) for row in rows]
    divisor, unit = _time_units(raw)
    origin = min(int(row["sent_monotonic_ns"]) for row in rows)
    destinations = sorted({_node(row, "destination") for row in rows})
    fig, (timeline, distribution) = plt.subplots(
        1, 2, figsize=(14, 5.8), constrained_layout=True)
    grouped: list[list[float]] = []
    for color, destination in zip(cycle(COLORS), destinations):
        selected = [row for row in rows if _node(
            row, "destination") == destination]
        x = [(int(row["sent_monotonic_ns"]) - origin) / 1e9 for row in selected]
        values = [float(row["transport_rtt_us"]) / divisor for row in selected]
        grouped.append(values)
        timeline.scatter(x, values, s=23, color=color,
                         alpha=0.8, label=destination)
    timeline.set(title=title, xlabel="Elapsed time (s)",
                 ylabel=f"Host observed RTT ({unit})")
    timeline.legend(frameon=False)
    _poster_boxplot(distribution, grouped, destinations)
    distribution.set(title="RTT by logical destination",
                     xlabel="Logical node", ylabel=f"Host observed RTT ({unit})")
    scaled = [value / divisor for value in raw]
    _annotate_summary(timeline, scaled, unit)
    _annotate_summary(distribution, scaled, unit)
    for axis in (timeline, distribution):
        _plain(axis)
    _save(fig, output, name, outputs)


def _plot_command_stages(commands: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    rows = [row for row in commands if row.get("sent_monotonic_ns")]
    if not rows:
        return
    available = [
        column
        for column in ("queue_delay_us", "gatt_write_duration_us", "write_complete_to_response_us")
        if any(row.get(column) for row in rows)
    ]
    if not available:
        pass
    else:
        all_values = [float(row[column])
                      for row in rows for column in available if row.get(column)]
        divisor, unit = _time_units(all_values)
        labels = {
            "queue_delay_us": "Host transmit queue",
            "gatt_write_duration_us": "Bleak GATT write call",
            "write_complete_to_response_us": "GATT write return to response",
        }
        origin = min(int(row["sent_monotonic_ns"]) for row in rows)
        fig, axes = plt.subplots(len(available), 1, figsize=(
            12, 3.1 * len(available)), sharex=True, constrained_layout=True)
        axes_array = np.atleast_1d(axes)
        opcodes = sorted({row.get("opcode") or "unknown" for row in rows})
        colors = {opcode: color for opcode,
                  color in zip(opcodes, cycle(COLORS))}
        for axis, column in zip(axes_array, available):
            summary_values = [
                float(row[column]) / divisor for row in rows if row.get(column)
            ]
            for opcode in opcodes:
                selected = [row for row in rows if (
                    row.get("opcode") or "unknown") == opcode and row.get(column)]
                x = [(int(row["sent_monotonic_ns"]) - origin) /
                     1e9 for row in selected]
                y = [float(row[column]) / divisor for row in selected]
                axis.scatter(x, y, s=20, alpha=0.78,
                             color=colors[opcode], label=opcode)
            axis.set(title=labels[column], ylabel=unit)
            _annotate_summary(axis, summary_values, unit)
            _plain(axis)
        axes_array[-1].set_xlabel("Elapsed time (s)")
        axes_array[0].legend(frameon=False, ncol=min(4, len(opcodes)))
        _save(fig, output, "command_host_stage_timing.png", outputs)

    origin = min(int(row["sent_monotonic_ns"]) for row in rows)

    korora_columns = [
        ("korora_receive_to_queue_us", "Korora receive to response queue"),
        ("korora_queue_to_tx_start_us", "Korora response queue wait"),
        ("korora_notify_duration_us", "Korora BLE notification duration"),
    ]
    korora_rows = [
        row
        for row in rows
        if any(row.get(column) for column, _ in korora_columns)
    ]
    if korora_rows:
        values = [
            float(row[column])
            for row in korora_rows
            for column, _ in korora_columns
            if row.get(column)
        ]
        divisor, unit = _time_units(values)
        fig, axes = plt.subplots(3, 1, figsize=(
            12, 9), sharex=True, constrained_layout=True)
        opcodes = sorted(
            {row.get("opcode") or "unknown" for row in korora_rows})
        colors = {opcode: color for opcode,
                  color in zip(opcodes, cycle(COLORS))}
        for axis, (column, title) in zip(axes, korora_columns):
            summary_values = [
                float(row[column]) / divisor
                for row in korora_rows
                if row.get(column)
            ]
            for opcode in opcodes:
                selected = [
                    row
                    for row in korora_rows
                    if (row.get("opcode") or "unknown") == opcode
                    and row.get(column)
                ]
                axis.scatter(
                    [(int(row["sent_monotonic_ns"]) - origin) /
                     1e9 for row in selected],
                    [float(row[column]) / divisor for row in selected],
                    s=20,
                    alpha=0.78,
                    color=colors[opcode],
                    label=opcode,
                )
            axis.set(title=title, ylabel=unit)
            _annotate_summary(axis, summary_values, unit)
            _plain(axis)
        axes[-1].set_xlabel("Elapsed time (s)")
        axes[0].legend(frameon=False, ncol=min(4, len(opcodes)))
        _save(fig, output, "command_korora_stage_timing.png", outputs)

    clock = [row for row in rows if row.get(
        "opcode") == "clock_exchange" and row.get("clock_full_exchange_us")]
    if clock:
        fig, axes = plt.subplots(2, 1, figsize=(
            11, 7), sharex=True, constrained_layout=True)
        x = [(int(row["sent_monotonic_ns"]) - origin) / 1e9 for row in clock]
        processing = [float(row["clock_server_processing_us"])
                      for row in clock if row.get("clock_server_processing_us")]
        network_raw = [float(row["clock_network_rtt_us"])
                       for row in clock if row.get("clock_network_rtt_us")]
        network_divisor, network_unit = _time_units(network_raw)
        network = [value / network_divisor for value in network_raw]
        process_rows = [row for row in clock if row.get(
            "clock_server_processing_us")]
        network_rows = [row for row in clock if row.get(
            "clock_network_rtt_us")]
        axes[0].scatter([(int(row["sent_monotonic_ns"]) - origin) /
                        1e9 for row in process_rows], processing, s=20, color=COLORS[3])
        axes[1].scatter([(int(row["sent_monotonic_ns"]) - origin) /
                        1e9 for row in network_rows], network, s=20, color=COLORS[0])
        axes[0].set(title="Korora clock exchange processing",
                    ylabel="t3 minus t2 (µs)")
        axes[1].set(title="Clock exchange outside Korora processing", xlabel="Elapsed time (s)",
                    ylabel=f"Full exchange minus processing ({network_unit})")
        _annotate_summary(axes[0], processing, "µs")
        _annotate_summary(axes[1], network, network_unit)
        for axis in axes:
            _plain(axis)
        _save(fig, output, "clock_exchange_stage_timing.png", outputs)


def _plot_command_groups(commands: list[dict[str, str]], results: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    if commands:
        groups = sorted({row.get("opcode") or "unknown" for row in commands})
        counts = Counter(row.get("opcode") or "unknown" for row in commands)
        missing = Counter(
            row.get("opcode") or "unknown"
            for row in commands
            if row.get("response_missing", "").lower() in {"true", "1"} or not row.get("status")
        )
        errors = Counter(
            row.get("opcode") or "unknown"
            for row in commands
            if row.get("status") not in {"", "ok", "accepted"}
        )
        x = np.arange(len(groups))
        fig, ax = plt.subplots(
            figsize=(max(10, len(groups) * 0.8), 5.7), constrained_layout=True)
        ax.bar(x, [counts[g] - missing[g] - errors[g]
               for g in groups], color=COLORS[3], label="OK")
        ax.bar(x, [errors[g] for g in groups], bottom=[counts[g] - missing[g] - errors[g]
               for g in groups], color=COLORS[4], label="Error status")
        ax.bar(x, [missing[g] for g in groups], bottom=[counts[g] - missing[g]
               for g in groups], color=COLORS[5], label="Missing response")
        ax.set(title="Command responses by opcode", xlabel="Opcode",
               ylabel="Commands", xticks=x, xticklabels=groups)
        ax.tick_params(axis="x", rotation=35)
        ax.legend(frameon=False)
        _plain(ax)
        _save(fig, output, "command_status_by_opcode.png", outputs)

        rtt_groups = [
            group
            for group in groups
            if any(
                row.get("transport_rtt_us")
                for row in commands
                if (row.get("opcode") or "unknown") == group
            )
        ]
        if rtt_groups:
            raw = [
                float(row["transport_rtt_us"])
                for row in commands
                if row.get("transport_rtt_us")
            ]
            divisor, unit = _time_units(raw)
            grouped = [
                [
                    float(row["transport_rtt_us"]) / divisor
                    for row in commands
                    if (row.get("opcode") or "unknown") == group
                    and row.get("transport_rtt_us")
                ]
                for group in rtt_groups
            ]
            fig, ax = plt.subplots(
                figsize=(max(10, len(rtt_groups) * 0.9), 5.7),
                constrained_layout=True,
            )
            _poster_boxplot(ax, grouped, rtt_groups)
            ax.set(
                title="Host observed RTT by opcode",
                xlabel="Opcode",
                ylabel=f"RTT ({unit})",
            )
            _annotate_summary(
                ax, [value for group in grouped for value in group], unit
            )
            ax.tick_params(axis="x", rotation=35)
            _plain(ax)
            _save(fig, output, "command_rtt_by_opcode.png", outputs)

    latency_rows = [row for row in results if row.get(
        "result_receive_latency_us")]
    if latency_rows:
        raw = [float(row["result_receive_latency_us"]) for row in latency_rows]
        divisor, unit = _time_units(raw)
        nodes = sorted({_node(row) for row in latency_rows})
        grouped = [
            [float(row["result_receive_latency_us"]) /
             divisor for row in latency_rows if _node(row) == node]
            for node in nodes
        ]
        fig, ax = plt.subplots(figsize=(10, 5.5), constrained_layout=True)
        _poster_boxplot(ax, grouped, nodes)
        ax.set(title="Command result event arrival by logical Fairy",
               xlabel="Logical node", ylabel=f"Command send to result event ({unit})")
        _annotate_summary(
            ax, [value for group in grouped for value in group], unit
        )
        _plain(ax)
        _save(fig, output, "command_result_latency_by_node.png", outputs)


def _plot_records(records: list[dict[str, str]], events: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    timed = [row for row in records if row.get("received_monotonic_ns")]
    if timed:
        origin = min(int(row["received_monotonic_ns"]) for row in timed)
        nodes = sorted({_node(row) for row in timed})
        fig, ax = plt.subplots(figsize=(11, 5.5), constrained_layout=True)
        all_rates: list[float] = []
        for color, node in zip(cycle(COLORS), nodes):
            seconds = np.asarray(
                [(int(row["received_monotonic_ns"]) - origin) / 1e9 for row in timed if _node(row) == node])
            if not len(seconds):
                continue
            stop = max(1, int(math.ceil(float(seconds.max()))) + 1)
            bins = np.arange(0, stop + 1, 1.0)
            counts, edges = np.histogram(seconds, bins=bins)
            all_rates.extend(counts.astype(float).tolist())
            ax.step(edges[:-1], counts, where="post",
                    color=color, linewidth=1.4, label=node)
        ax.set(title="Received record rate by logical node",
               xlabel="Elapsed time (s)", ylabel="Records per 1 s bin")
        ax.legend(frameon=False, ncol=min(4, len(nodes)))
        _annotate_summary(ax, all_rates, "records/s")
        _plain(ax)
        _save(fig, output, "record_rate_by_node.png", outputs)

    if events:
        nodes = sorted({_node(row) for row in events})
        types = sorted({row.get("record_type") or "unknown" for row in events})
        x = np.arange(len(nodes))
        bottom = np.zeros(len(nodes))
        fig, ax = plt.subplots(
            figsize=(max(10, len(nodes) * 1.2), 6), constrained_layout=True)
        for color, event_type in zip(cycle(COLORS), types):
            values = np.asarray([sum(1 for row in events if _node(
                row) == node and row.get("record_type") == event_type) for node in nodes])
            ax.bar(x, values, bottom=bottom, color=color, label=event_type)
            bottom += values
        ax.set(title="Application events by logical node",
               xlabel="Logical node", ylabel="Events", xticks=x, xticklabels=nodes)
        ax.legend(frameon=False, ncol=min(4, len(types)))
        _plain(ax)
        _save(fig, output, "event_counts_by_node.png", outputs)

        timed_events = [row for row in events if row.get(
            "received_monotonic_ns")]
        if timed_events:
            origin = min(int(row["received_monotonic_ns"])
                         for row in timed_events)
            type_colors = {
                event_type: color
                for event_type, color in zip(types, cycle(COLORS))
            }
            node_index = {node: index for index, node in enumerate(nodes)}
            fig, ax = plt.subplots(
                figsize=(12, max(5.5, len(nodes) * 0.75)), constrained_layout=True)
            for event_type in types:
                selected = [row for row in timed_events if row.get(
                    "record_type") == event_type]
                ax.scatter(
                    [(int(row["received_monotonic_ns"]) -
                      origin) / 1e9 for row in selected],
                    [node_index[_node(row)] for row in selected],
                    s=25,
                    alpha=0.78,
                    color=type_colors[event_type],
                    label=event_type,
                )
            ax.set(
                title="Application event timeline",
                xlabel="Elapsed time (s)",
                ylabel="Logical node",
                yticks=range(len(nodes)),
                yticklabels=nodes,
            )
            ax.legend(frameon=False, ncol=min(4, len(types)))
            _save(fig, output, "event_timeline_by_node.png", outputs)


def _plot_health(rows: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    timed = [row for row in rows if row.get("received_monotonic_ns")]
    if not timed:
        return
    origin = min(int(row["received_monotonic_ns"]) for row in timed)
    nodes = sorted({_node(row) for row in timed})
    fig, axes = plt.subplots(3, 1, figsize=(
        11, 9), sharex=True, constrained_layout=True)
    all_queue: list[float] = []
    all_dropped: list[float] = []
    all_errors: list[float] = []
    for color, node in zip(cycle(COLORS), nodes):
        selected = [row for row in timed if _node(row) == node]
        x = [(int(row["received_monotonic_ns"]) - origin) / 1e9 for row in selected]
        queue = [_number(row, "queue_depth") or 0 for row in selected]
        dropped = [_number(row, "dropped_records") or 0 for row in selected]
        errors = [_number(row, "transport_errors") or 0 for row in selected]
        all_queue.extend(queue)
        all_dropped.extend(dropped)
        all_errors.extend(errors)
        axes[0].step(x, queue, where="post", color=color, label=node)
        axes[1].step(x, dropped, where="post", color=color, label=node)
        axes[2].step(x, errors, where="post", color=color, label=node)
    axes[0].set(title="Node output queue depth", ylabel="Records")
    axes[1].set(title="Cumulative dropped records", ylabel="Dropped")
    axes[2].set(title="Cumulative transport errors",
                xlabel="Elapsed time (s)", ylabel="Errors")
    axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
    _annotate_summary(axes[0], all_queue, "records")
    _annotate_summary(axes[1], all_dropped, "records")
    _annotate_summary(axes[2], all_errors, "errors")
    for axis in axes:
        _plain(axis)
    _save(fig, output, "node_health_timeline.png", outputs)

    rssi_rows = [row for row in timed if row.get("rssi_dbm")]
    if rssi_rows:
        fig, ax = plt.subplots(figsize=(11, 5.5), constrained_layout=True)
        all_rssi: list[float] = []
        for color, node in zip(cycle(COLORS), nodes):
            selected = [row for row in rssi_rows if _node(row) == node]
            x = [(int(row["received_monotonic_ns"]) -
                  origin) / 1e9 for row in selected]
            y = [float(row["rssi_dbm"]) for row in selected]
            all_rssi.extend(y)
            ax.plot(x, y, color=color, linewidth=1.4, label=node)
        ax.set(title="Link RSSI by logical node",
               xlabel="Elapsed time (s)", ylabel="RSSI (dBm)")
        ax.legend(frameon=False)
        _annotate_summary(ax, all_rssi, "dBm")
        _plain(ax)
        _save(fig, output, "link_rssi_by_node.png", outputs)


def _plot_clock_continuity(
    records: list[dict[str, str]], health: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    continuity_types = {"health", "link_quality", "sync_quality"}
    timed = [
        row
        for row in records
        if row.get("received_monotonic_ns")
        and row.get("timestamp_ticks")
        and row.get("clock_hz")
        and float(row.get("clock_hz") or 0) > 0
        and row.get("record_type") in continuity_types
    ]
    uptime = [row for row in health if row.get(
        "received_monotonic_ns") and row.get("uptime_ms")]
    if not timed and not uptime:
        return
    fig, axes = plt.subplots(2, 1, figsize=(13, 8), constrained_layout=True)
    all_timestamp_residuals: list[float] = []
    all_uptime_residuals: list[float] = []
    if timed:
        origin = min(int(float(row["received_monotonic_ns"])) for row in timed)
        nodes = sorted({_node(row) for row in timed})
        for color, node in zip(cycle(COLORS), nodes):
            node_rows = [row for row in timed if _node(row) == node]
            selected_type = next(
                (
                    record_type
                    for record_type in ("health", "link_quality", "sync_quality")
                    if sum(1 for row in node_rows if row.get("record_type") == record_type) >= 5
                ),
                None,
            )
            if selected_type is None:
                continue
            selected = sorted(
                [row for row in node_rows if row.get(
                    "record_type") == selected_type],
                key=lambda row: int(float(row["received_monotonic_ns"])),
            )
            first_host = int(float(selected[0]["received_monotonic_ns"]))
            first_local = float(
                selected[0]["timestamp_ticks"]) / float(selected[0]["clock_hz"])
            x = [(int(float(row["received_monotonic_ns"])) -
                  origin) / 1e9 for row in selected]
            residual = [
                (
                    (float(row["timestamp_ticks"]) /
                     float(row["clock_hz"]) - first_local)
                    - (int(float(row["received_monotonic_ns"])
                           ) - first_host) / 1e9
                )
                * 1000
                for row in selected
            ]
            all_timestamp_residuals.extend(residual)
            axes[0].plot(
                x,
                residual,
                color=color,
                linewidth=1.0,
                alpha=0.82,
                label=f"{node} ({selected_type})",
            )
        axes[0].set(title="Current record timestamp continuity versus host arrival",
                    xlabel="Elapsed host time (s)", ylabel="Relative timestamp minus arrival (ms)")
        axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
        _annotate_summary(
            axes[0], all_timestamp_residuals, "ms", signed=True
        )
    if uptime:
        origin = min(int(float(row["received_monotonic_ns"]))
                     for row in uptime)
        nodes = sorted({_node(row) for row in uptime})
        for color, node in zip(cycle(COLORS), nodes):
            selected = sorted([row for row in uptime if _node(
                row) == node], key=lambda row: int(float(row["received_monotonic_ns"])))
            first_host = int(float(selected[0]["received_monotonic_ns"]))
            first_uptime = float(selected[0]["uptime_ms"])
            residual = [
                (float(row["uptime_ms"]) - first_uptime)
                - (int(float(row["received_monotonic_ns"])
                       ) - first_host) / 1e6
                for row in selected
            ]
            all_uptime_residuals.extend(residual)
            axes[1].plot(
                [(int(float(row["received_monotonic_ns"])) -
                  origin) / 1e9 for row in selected],
                residual,
                color=color,
                linewidth=1.2,
                label=node,
            )
        axes[1].set(title="Reported uptime continuity versus host arrival",
                    xlabel="Elapsed host time (s)", ylabel="Relative uptime minus arrival (ms)")
        axes[1].legend(frameon=False, ncol=min(4, len(nodes)))
        _annotate_summary(axes[1], all_uptime_residuals, "ms", signed=True)
    for axis in axes:
        _plain(axis)
    _save(fig, output, "clock_and_delivery_continuity.png", outputs)


def _plot_ttl(rows: list[dict[str, str]], output: Path, outputs: list[Path]) -> None:
    selected = [row for row in rows if row.get("total_error_ns")]
    if not selected:
        return
    values = [float(row["total_error_ns"]) / 1_000 for row in selected]
    sequences = [int(float(row.get("sequence") or index + 1))
                 for index, row in enumerate(selected)]
    fig, (timeline, distribution) = plt.subplots(
        1, 2, figsize=(13, 5.5), constrained_layout=True)

    # Keep individual captures visible without letting dense markers obscure
    # the overall TTL-alignment trend.
    timeline.scatter(sequences, values, s=24, color=COLORS[2], alpha=0.25)

    if len(values) >= 5:
        order = np.argsort(np.asarray(sequences))
        ordered_sequences = np.asarray(sequences, dtype=float)[order]
        ordered_values = np.asarray(values, dtype=float)[order]

        # Use an adaptive odd-sized window: broad enough to suppress visual
        # noise, but capped so local changes remain visible in long runs.
        window = min(51, max(5, int(round(len(ordered_values) * 0.09))))
        if window % 2 == 0:
            window += 1
        half_window = window // 2

        running_median: list[float] = []
        lower_band: list[float] = []
        upper_band: list[float] = []
        for index in range(len(ordered_values)):
            start = max(0, index - half_window)
            stop = min(len(ordered_values), index + half_window + 1)
            local = ordered_values[start:stop]
            running_median.append(float(np.median(local)))
            lower_band.append(float(np.percentile(local, 25)))
            upper_band.append(float(np.percentile(local, 75)))

        timeline.fill_between(
            ordered_sequences,
            lower_band,
            upper_band,
            color=COLORS[2],
            alpha=0.16,
            linewidth=0,
        )
        timeline.plot(
            ordered_sequences,
            running_median,
            color=PALETTE["reset"],
            linewidth=2.4,
        )
        timeline.legend(frameon=False)

    timeline.axhline(0, color=PALETTE["muted"], linewidth=1, alpha=0.8)
    timeline.set(title="Galapagos TTL alignment",
                 xlabel="TTL sequence", ylabel="Capture minus target (µs)")
    distribution.hist(values, bins=min(45, max(8, len(values) // 3)),
                      color=COLORS[2], alpha=0.75,
                      edgecolor=PALETTE["panel"])
    distribution.set(title="TTL error distribution",
                     xlabel="Capture minus target (µs)", ylabel="Count")
    _annotate_summary(timeline, values, "µs", signed=True)
    _annotate_summary(distribution, values, "µs", signed=True)
    for axis in (timeline, distribution):
        _plain(axis)
    _save(fig, output, "ttl_alignment_detail.png", outputs)


def _truthy(value: str | None) -> bool:
    return str(value or "").lower() in {"true", "1", "yes"}


def _plot_record_gaps(
    rows: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    rows = [row for row in rows if row.get(
        "gap_s") and row.get("end_elapsed_s")]
    if not rows:
        return
    nodes = sorted({row.get("source_node") or "unknown" for row in rows})
    fig, axes = plt.subplots(2, 1, figsize=(12, 8), constrained_layout=True)
    for color, node in zip(cycle(COLORS), nodes):
        selected = [row for row in rows if (
            row.get("source_node") or "unknown") == node]
        x = [float(row["end_elapsed_s"]) for row in selected]
        gaps = [float(row["gap_s"]) for row in selected]
        normal_x = [value for value, row in zip(
            x, selected) if not _truthy(row.get("is_anomaly"))]
        normal_y = [value for value, row in zip(
            gaps, selected) if not _truthy(row.get("is_anomaly"))]
        anomaly_x = [value for value, row in zip(
            x, selected) if _truthy(row.get("is_anomaly"))]
        anomaly_y = [value for value, row in zip(
            gaps, selected) if _truthy(row.get("is_anomaly"))]
        axes[0].scatter(normal_x, normal_y, s=11,
                        alpha=0.38, color=color, label=node)
        axes[0].scatter(anomaly_x, anomaly_y, s=42, marker="x",
                        linewidths=1.8, color=color)
        ordered = np.sort(np.asarray(gaps, dtype=float))
        axes[1].step(ordered, np.arange(1, len(ordered) + 1) /
                     len(ordered), where="post", color=color, label=node)
    axes[0].set_yscale("log")
    axes[0].set(title="Record interarrival gaps",
                xlabel="Elapsed time (s)", ylabel="Gap (s, log scale)")
    axes[1].set_xscale("log")
    axes[1].set(title="Record gap empirical CDF",
                xlabel="Gap (s, log scale)", ylabel="Fraction at or below gap")
    axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
    all_gaps = [float(row["gap_s"]) for row in rows]
    _annotate_summary(axes[0], all_gaps, "s")
    _annotate_summary(axes[1], all_gaps, "s")
    _save(fig, output, "record_gap_timeline_and_cdf.png", outputs)

    longest = sorted(rows, key=lambda row: float(
        row["gap_s"]), reverse=True)[:25]
    if longest:
        labels = [
            f"{row.get('source_node', 'unknown')} @ {float(row['start_elapsed_s']):.1f}s"
            for row in reversed(longest)
        ]
        values = [float(row["gap_s"]) for row in reversed(longest)]
        fig, ax = plt.subplots(
            figsize=(11, max(5.5, 0.3 * len(longest))), constrained_layout=True)
        ax.barh(labels, values, color=COLORS[0], alpha=0.82)
        ax.set(title="Longest record silences",
               xlabel="Silence duration (s)", ylabel="Node and start time")
        _annotate_summary(ax, all_gaps, "s")
        _plain(ax, "x")
        _save(fig, output, "longest_record_silences.png", outputs)


def _plot_record_type_matrix(
    rows: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    if not rows:
        return
    nodes = sorted({row.get("source_node") or "unknown" for row in rows})
    types = sorted({row.get("record_type") or "unknown" for row in rows})
    values = np.zeros((len(types), len(nodes)), dtype=float)
    for row in rows:
        node = row.get("source_node") or "unknown"
        record_type = row.get("record_type") or "unknown"
        values[types.index(record_type), nodes.index(
            node)] = float(row.get("rate_hz") or 0)
    fig, ax = plt.subplots(
        figsize=(max(8, 1.3 * len(nodes)), max(6, 0.42 * len(types))),
        constrained_layout=True,
    )
    image = ax.imshow(np.log10(values + 1e-4), aspect="auto", cmap=POSTER_CMAP)
    ax.set(
        title="Record rates by node and record type",
        xlabel="Logical node",
        ylabel="Record type",
        xticks=range(len(nodes)),
        xticklabels=nodes,
        yticks=range(len(types)),
        yticklabels=types,
    )
    for row_index in range(len(types)):
        for column_index in range(len(nodes)):
            value = values[row_index, column_index]
            if value > 0:
                normalized = image.norm(math.log10(value + 1e-4))
                ax.text(column_index, row_index,
                        f"{value:.2g}", ha="center", va="center", fontsize=7,
                        color=PALETTE["ink"] if normalized < 0.48 else PALETTE["panel"])
    colorbar = fig.colorbar(image, ax=ax)
    colorbar.set_label("log10 records/s")
    _save(fig, output, "record_rate_matrix.png", outputs)


def _plot_counter_deltas(
    rows: list[dict[str, str]], anomalies: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    timed = [row for row in rows if row.get(
        "elapsed_s") and row.get("rate_per_s")]
    if not timed:
        return
    interesting = [
        row
        for row in timed
        if float(row.get("delta") or 0) > 0 or _truthy(row.get("counter_reset"))
    ]
    if interesting:
        metrics = sorted(
            {row.get("metric") or "unknown" for row in interesting})
        nodes = sorted(
            {row.get("source_node") or "unknown" for row in interesting})
        fig, axes = plt.subplots(2, 1, figsize=(
            13, 8), constrained_layout=True)
        for color, node in zip(cycle(COLORS), nodes):
            selected = [row for row in interesting if (
                row.get("source_node") or "unknown") == node]
            axes[0].scatter(
                [float(row["elapsed_s"]) for row in selected],
                [float(row["rate_per_s"]) for row in selected],
                s=18,
                alpha=0.7,
                color=color,
                label=node,
            )
        totals = Counter()
        for row in interesting:
            if not _truthy(row.get("counter_reset")):
                totals[(row.get("source_node") or "unknown", row.get(
                    "metric") or "unknown")] += float(row.get("delta") or 0)
        labels = [f"{node}\n{metric}" for node, metric in totals]
        axes[1].bar(np.arange(len(labels)), list(totals.values()), color=[
                    COLORS[index % len(COLORS)] for index in range(len(labels))])
        axes[1].set_xticks(np.arange(len(labels)), labels,
                           rotation=55, ha="right", fontsize=8)
        axes[0].set(title="Positive counter increments",
                    xlabel="Elapsed time (s)", ylabel="Increment rate (/s)")
        axes[1].set(title="Total increments during recording",
                    ylabel="Counter increment")
        axes[0].legend(frameon=False, ncol=min(4, len(nodes)))
        _annotate_summary(
            axes[0], [float(row["rate_per_s"]) for row in interesting], "/s"
        )
        for axis in axes:
            _plain(axis)
        _save(fig, output, "counter_increment_diagnostics.png", outputs)

    drop_bursts = [
        row
        for row in anomalies
        if row.get("category") == "record_loss_burst"
        and row.get("metric") in {"dropped_records", "korora_ble_dropped"}
        and row.get("source_node") in {"korora", "adelie_host"}
    ]
    if any(row.get("source_node") == "adelie_host" for row in drop_bursts):
        drop_bursts = [
            row for row in drop_bursts if row.get("source_node") == "adelie_host"
        ]
    if drop_bursts:
        fig, axes = plt.subplots(3, 1, figsize=(
            11, 9), constrained_layout=True)
        starts = [float(row["start_s"]) for row in drop_bursts]
        deltas = [float(row["delta"]) for row in drop_bursts]
        axes[0].scatter(starts, deltas, s=55, color=COLORS[5])
        axes[0].set(title="Korora BLE outbound drop bursts",
                    xlabel="Elapsed time (s)", ylabel="Records dropped")
        _annotate_summary(axes[0], deltas, "records")
        for axis, period, key in (
            (axes[1], 512.0, "phase_512_s"),
            (axes[2], 1024.0, "phase_1024_s"),
        ):
            phases = [float(row[key]) for row in drop_bursts]
            axis.scatter(phases, deltas, s=55, color=COLORS[0])
            axis.set(title=f"Drop burst phase within {period:g} s period",
                     xlabel="Phase (s)", ylabel="Records dropped", xlim=(0, period))
            _annotate_summary(axis, deltas, "records")
        _save(fig, output, "korora_drop_burst_periodicity.png", outputs)


def _plot_host_transport(
    rows: list[dict[str, str]], deltas: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    timed = [row for row in rows if row.get(
        "received_monotonic_ns") or row.get("sample_ns")]
    if not timed:
        return
    timestamps = [int(float(row.get("received_monotonic_ns")
                      or row.get("sample_ns") or 0)) for row in timed]
    origin = min(timestamps)
    x = [(value - origin) / 1e9 for value in timestamps]
    fig, axes = plt.subplots(4, 1, figsize=(
        13, 11), sharex=True, constrained_layout=True)
    for column, color in (("outgoing_queue", COLORS[0]), ("last_rx_age_ms", COLORS[5]), ("last_tx_age_ms", COLORS[2])):
        selected = [(elapsed, _number(row, column))
                    for elapsed, row in zip(x, timed)]
        selected = [(elapsed, value)
                    for elapsed, value in selected if value is not None]
        if selected:
            axes[0].plot([item[0] for item in selected], [item[1]
                         for item in selected], linewidth=1.25, color=color, label=column)
    axes[0].set(title="Adelie host BLE queue and inactivity",
                ylabel="Queue / age (ms)")
    axes[0].legend(frameon=False, ncol=3)

    host_delta = [row for row in deltas if row.get(
        "source_node") == "adelie_host" and float(row.get("delta") or 0) > 0]
    throughput_values: list[float] = []
    for column, color in (("rx_bytes", COLORS[2]), ("tx_bytes", COLORS[4])):
        selected = [row for row in host_delta if row.get("metric") == column]
        throughput_values.extend(float(row["rate_per_s"]) for row in selected)
        axes[1].plot([float(row["elapsed_s"]) for row in selected], [float(
            row["rate_per_s"]) for row in selected], color=color, linewidth=1.25, label=column)
    axes[1].set(title="Adelie BLE throughput", ylabel="Bytes/s")
    axes[1].legend(frameon=False)
    _annotate_summary(axes[1], throughput_values, "bytes/s")

    error_metrics = ("rx_decode_errors", "rx_reassembly_errors",
                     "rx_address_errors", "tx_write_errors", "korora_ble_dropped")
    error_values: list[float] = []
    for color, metric in zip(cycle(COLORS), error_metrics):
        selected = [row for row in host_delta if row.get("metric") == metric]
        if selected:
            error_values.extend(float(row["rate_per_s"]) for row in selected)
            axes[2].plot([float(row["elapsed_s"]) for row in selected], [float(
                row["rate_per_s"]) for row in selected], color=color, linewidth=1.2, label=metric)
    axes[2].set(title="Host BLE and Korora outbound errors",
                ylabel="New errors/s")
    axes[2].legend(frameon=False, ncol=3)
    _annotate_summary(axes[2], error_values, "errors/s")

    clock_values: list[float] = []
    for column, color in (("clock_last_rtt_us", COLORS[0]), ("clock_median_rtt_us", COLORS[1]), ("clock_rms_us", COLORS[3])):
        selected = [(elapsed, _number(row, column))
                    for elapsed, row in zip(x, timed)]
        selected = [(elapsed, value)
                    for elapsed, value in selected if value is not None]
        if selected:
            clock_values.extend(item[1] / 1000 for item in selected)
            axes[3].plot([item[0] for item in selected], [
                         item[1] / 1000 for item in selected], color=color, linewidth=1.25, label=column)
    axes[3].set(title="Adelie to Korora clock exchange",
                xlabel="Elapsed time (s)", ylabel="Time (ms)")
    axes[3].legend(frameon=False, ncol=3)
    _annotate_summary(axes[3], clock_values, "ms")
    for axis in axes:
        _plain(axis)
    _save(fig, output, "host_ble_full_diagnostics.png", outputs)


def _plot_anomaly_overview(
    rows: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    if not rows:
        return
    categories = sorted({row.get("category") or "unknown" for row in rows})
    category_index = {category: index for index,
                      category in enumerate(categories)}
    fig, axes = plt.subplots(2, 1, figsize=(13, 8), constrained_layout=True)
    for color, category in zip(cycle(COLORS), categories):
        selected = [row for row in rows if (
            row.get("category") or "unknown") == category]
        for row in selected:
            start = float(row["start_s"])
            end = float(row["end_s"])
            axes[0].plot([start, max(start + 0.02, end)], [category_index[category]]
                         * 2, linewidth=5, color=color, solid_capstyle="butt")
    axes[0].set(title="Automatically detected verification anomalies", xlabel="Elapsed time (s)",
                ylabel="Category", yticks=range(len(categories)), yticklabels=categories)
    counts = Counter(row.get("category") or "unknown" for row in rows)
    axes[1].bar(counts.keys(), counts.values(), color=[
                COLORS[index % len(COLORS)] for index in range(len(counts))])
    axes[1].set(title="Anomaly counts", xlabel="Category", ylabel="Intervals")
    axes[1].tick_params(axis="x", rotation=35)
    _plain(axes[0], "x")
    _plain(axes[1])
    _save(fig, output, "verification_anomaly_overview.png", outputs)


def _plot_correlations(
    rows: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    rows = [row for row in rows if row.get("pearson_r")]
    if not rows:
        return
    strongest = sorted(rows, key=lambda row: float(
        row.get("absolute_r") or 0), reverse=True)[:25]
    labels = [
        f"{row['feature_a']} × {row['feature_b']}" for row in reversed(strongest)]
    values = [float(row["pearson_r"]) for row in reversed(strongest)]
    fig, ax = plt.subplots(
        figsize=(12, max(7, 0.35 * len(strongest))), constrained_layout=True)
    colors = [COLORS[5] if value < 0 else COLORS[3] for value in values]
    ax.barh(labels, values, color=colors, alpha=0.86)
    ax.axvline(0, color=PALETTE["muted"], linewidth=0.9, alpha=0.8)
    ax.set(title="Strongest one second metric correlations",
           xlabel="Pearson correlation", ylabel="Metric pair", xlim=(-1, 1))
    _save(fig, output, "verification_metric_correlations.png", outputs)


def _plot_command_deep(
    rows: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    selected = [row for row in rows if row.get("transport_rtt_us")]
    if not selected:
        return
    opcodes = sorted({row.get("opcode") or "unknown" for row in selected})
    fig, axes = plt.subplots(2, 1, figsize=(12, 8), constrained_layout=True)
    for color, opcode in zip(cycle(COLORS), opcodes):
        values = np.sort(np.asarray([float(row["transport_rtt_us"]) /
                         1000 for row in selected if (row.get("opcode") or "unknown") == opcode]))
        axes[0].step(values, np.arange(1, len(values) + 1) /
                     len(values), where="post", color=color, label=opcode)
        axes[1].hist(values, bins=min(60, max(10, len(values) // 5)),
                     histtype="step", linewidth=1.4, color=color, label=opcode)
    axes[0].set(title="Command RTT empirical CDF",
                xlabel="RTT (ms)", ylabel="Fraction at or below RTT")
    axes[1].set(title="Command RTT distribution",
                xlabel="RTT (ms)", ylabel="Count")
    axes[0].legend(frameon=False, ncol=min(4, len(opcodes)))
    all_rtt = [float(row["transport_rtt_us"]) / 1000 for row in selected]
    _annotate_summary(axes[0], all_rtt, "ms")
    _annotate_summary(axes[1], all_rtt, "ms")
    for axis in axes:
        _plain(axis, "x")
    _save(fig, output, "command_rtt_cdf_and_histogram.png", outputs)


def _plot_sync_distributions(
    rows: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    if not rows:
        return
    nodes = sorted({_node(row) for row in rows})
    rms_groups = [[float(row["rms_ns"]) / 1000 for row in rows if _node(row)
                   == node and row.get("rms_ns")] for node in nodes]
    skew_groups = [[float(row["skew_ppb"]) / 1000 for row in rows if _node(row)
                    == node and row.get("skew_ppb")] for node in nodes]
    fig, axes = plt.subplots(1, 2, figsize=(13, 5.8), constrained_layout=True)
    _poster_boxplot(axes[0], rms_groups, nodes)
    _poster_boxplot(axes[1], skew_groups, nodes)
    axes[0].set(title="Synchronization RMS distribution",
                xlabel="Logical node", ylabel="RMS (µs)")
    axes[1].set(title="Clock skew distribution",
                xlabel="Logical node", ylabel="Skew (ppm)")
    _annotate_summary(
        axes[0], [value for group in rms_groups for value in group], "µs"
    )
    _annotate_summary(
        axes[1], [value for group in skew_groups for value in group],
        "ppm", signed=True
    )
    for axis in axes:
        axis.tick_params(axis="x", rotation=25)
        _plain(axis)
    _save(fig, output, "sync_quality_distributions.png", outputs)


def _plot_ttl_completeness(
    rows: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    if not rows:
        return
    stages = (
        "target_korora_ticks",
        "generated_local_ticks",
        "generated_korora_ticks",
        "captured_korora_ticks",
        "total_error_ns",
    )
    counts = [sum(1 for row in rows if row.get(stage)) for stage in stages]
    fig, axes_grid = plt.subplots(
        2, 2, figsize=(13, 9), constrained_layout=True)
    axes = axes_grid.ravel()
    axes[0].bar(stages, counts, color=COLORS[: len(stages)])
    axes[0].tick_params(axis="x", rotation=30)
    axes[0].set(title="TTL sequence stage completeness",
                xlabel="Stage", ylabel="Sequences")
    errors = [float(row["total_error_ns"]) /
              1000 for row in rows if row.get("total_error_ns")]
    if errors:
        ordered = np.sort(np.abs(np.asarray(errors)))
        axes[1].step(ordered, np.arange(1, len(ordered) + 1) /
                     len(ordered), where="post", color=COLORS[2])
        axes[1].set(title="Absolute TTL error empirical CDF",
                    xlabel="Absolute error (µs)", ylabel="Fraction at or below error")
        _annotate_summary(axes[1], errors, "µs", signed=True)
        _plain(axes[1], "x")
    generation = [
        (int(float(row.get("sequence") or index)),
         float(row["generation_error_ns"]) / 1000)
        for index, row in enumerate(rows)
        if row.get("generation_error_ns")
    ]
    if generation:
        axes[2].scatter([item[0] for item in generation], [item[1]
                        for item in generation], s=20, color=COLORS[4])
        axes[2].axhline(0, color=PALETTE["muted"], linewidth=0.9, alpha=0.8)
        axes[2].set(title="Galapagos generation error",
                    xlabel="TTL sequence", ylabel="Generated minus target (µs)")
        _annotate_summary(
            axes[2], [item[1] for item in generation], "µs", signed=True
        )
    capture_latency = [
        (int(float(row.get("sequence") or index)), float(
            row["capture_after_generation_ns"]) / 1000)
        for index, row in enumerate(rows)
        if row.get("capture_after_generation_ns")
    ]
    if capture_latency:
        axes[3].scatter([item[0] for item in capture_latency], [item[1]
                        for item in capture_latency], s=20, color=COLORS[3])
        axes[3].set(title="Generated pulse to Korora capture",
                    xlabel="TTL sequence", ylabel="Capture minus generated (µs)")
        _annotate_summary(
            axes[3], [item[1] for item in capture_latency], "µs"
        )
    for axis in axes:
        _plain(axis)
    _save(fig, output, "ttl_completeness_and_cdf.png", outputs)


def _plot_anomaly_zooms(
    anomalies: list[dict[str, str]], timeseries: list[dict[str, str]], output: Path, outputs: list[Path]
) -> None:
    important = [
        row
        for row in anomalies
        if row.get("category") == "record_loss_burst"
    ][:16]
    if not important or not timeseries:
        return
    features = sorted(
        {key for row in timeseries for key in row if key != "elapsed_s"})
    record_features = [feature for feature in features if feature.startswith(
        "records_per_s__") and feature.count("__") == 1]
    rate_features = [
        feature for feature in features if feature.startswith("counter_rate__")]
    loss_features = [
        feature
        for feature in rate_features
        if feature.endswith("__dropped_records")
        or feature.endswith("__ttl_capture_drops")
    ]
    transport_features = [
        feature
        for feature in rate_features
        if feature not in loss_features
        and any(token in feature for token in ("error", "timeout", "retry"))
    ]
    regime_changes = [
        row for row in anomalies if row.get("category") == "command_rtt_regime_change"
    ]

    def active_features(
        candidates: list[str], selected_rows: list[dict[str, str]], limit: int
    ) -> list[str]:
        totals = []
        for feature in candidates:
            values = [float(row.get(feature) or 0) for row in selected_rows]
            if values and max(values) > 0:
                totals.append((sum(abs(value) for value in values), feature))
        return [feature for _, feature in sorted(totals, reverse=True)[:limit]]

    def short_counter_name(feature: str) -> str:
        return feature.replace("counter_rate__", "").replace("__", " ")

    for anomaly in important:
        start = float(anomaly["start_s"])
        end = float(anomaly["end_s"])
        margin = max(45.0, min(90.0, (end - start) * 0.75))
        selected = [row for row in timeseries if start -
                    margin <= float(row["elapsed_s"]) <= end + margin]
        if not selected:
            continue
        fig, axes = plt.subplots(4, 1, figsize=(
            13, 12), sharex=True, constrained_layout=True)
        for axis in axes:
            axis.axvspan(start, end, color=COLORS[5], alpha=0.16)
        for color, feature in zip(cycle(COLORS), record_features):
            axes[0].plot([float(row["elapsed_s"]) for row in selected], [float(row.get(feature) or 0)
                         for row in selected], color=color, linewidth=1.25, label=feature.replace("records_per_s__", ""))
        selected_loss = active_features(loss_features, selected, 6)
        for color, feature in zip(cycle(COLORS), selected_loss):
            axes[1].plot(
                [float(row["elapsed_s"]) for row in selected],
                [float(row.get(feature) or 0) for row in selected],
                color=color,
                linewidth=1.35,
                label=short_counter_name(feature),
            )
        selected_transport = active_features(transport_features, selected, 7)
        for color, feature in zip(cycle(COLORS), selected_transport):
            axes[2].plot(
                [float(row["elapsed_s"]) for row in selected],
                [float(row.get(feature) or 0) for row in selected],
                color=color,
                linewidth=1.2,
                label=short_counter_name(feature),
            )

        x_values = [float(row["elapsed_s"]) for row in selected]
        if "command_rtt_us" in features:
            axes[3].plot(
                x_values,
                [float(row.get("command_rtt_us") or math.nan) /
                 1000 for row in selected],
                color=COLORS[0],
                linewidth=1.25,
                label="Command RTT",
            )
        if "host__last_rx_age_ms" in features:
            axes[3].plot(
                x_values,
                [float(row.get("host__last_rx_age_ms") or math.nan)
                 for row in selected],
                color=COLORS[1],
                linewidth=1.1,
                label="Host receive age",
            )
        queue_axis = axes[3].twinx()
        if "host__outgoing_queue" in features:
            queue_axis.plot(
                x_values,
                [float(row.get("host__outgoing_queue") or math.nan)
                 for row in selected],
                color=COLORS[2],
                linewidth=1.1,
                label="Adelie command queue",
            )
        for change in regime_changes:
            change_time = float(change["start_s"])
            if x_values[0] <= change_time <= x_values[-1]:
                axes[3].axvline(
                    change_time,
                    color=COLORS[5],
                    linestyle="--",
                    linewidth=1.1,
                    alpha=0.8,
                )

        axes[0].set(
            title=f"Record loss incident {anomaly.get('anomaly_id')}: {anomaly.get('source_node')} {anomaly.get('metric')}", ylabel="Records/s")
        axes[1].set(ylabel="Newly dropped/s")
        axes[2].set(ylabel="Transport errors/s")
        axes[3].set(xlabel="Elapsed time (s)",
                    ylabel="RTT and receive age (ms)")
        queue_axis.set_ylabel("Adelie command queue")
        for axis in axes[:3]:
            if axis.lines:
                axis.legend(frameon=False, ncol=3, fontsize=8)
            _plain(axis)
        handles, labels = axes[3].get_legend_handles_labels()
        queue_handles, queue_labels = queue_axis.get_legend_handles_labels()
        if handles or queue_handles:
            axes[3].legend(
                handles + queue_handles,
                labels + queue_labels,
                frameon=False,
                ncol=3,
                fontsize=8,
            )
        _plain(axes[3])
        _plain(queue_axis)
        _save(
            fig, output, f"anomaly_{int(float(anomaly.get('anomaly_id') or 0)):03d}_zoom.png", outputs)


def create_plots(parsed_directory: Path, output_directory: Path | None = None) -> list[Path]:
    output_directory = output_directory or parsed_directory / "plots"
    output_directory.mkdir(parents=True, exist_ok=True)
    for pattern in ("anomaly_*_zoom.png", "anomaly_*_zoom.svg"):
        for stale_zoom in output_directory.glob(pattern):
            stale_zoom.unlink()
    _style()
    outputs: list[Path] = []

    quality = _read(parsed_directory / "sync_quality.csv")
    pairs = _read(parsed_directory / "sync_pairs.csv")
    predictions = _read(parsed_directory / "prediction_errors.csv")
    commands = _read(parsed_directory / "commands.csv")
    results = _read(parsed_directory / "command_results.csv")
    records = _read(parsed_directory / "records.csv")
    events = _read(parsed_directory / "events.csv")
    health = _read(parsed_directory / "health.csv")
    host_health = _read(parsed_directory / "host_transport_health.csv")
    counter_deltas = _read(parsed_directory / "counter_deltas.csv")
    record_gaps = _read(parsed_directory / "record_gaps.csv")
    record_types = _read(parsed_directory / "record_type_summary.csv")
    anomalies = _read(parsed_directory / "anomaly_intervals.csv")
    timeseries = _read(parsed_directory / "timeseries_1s.csv")
    correlations = _read(parsed_directory / "correlations.csv")
    ttl = _read(parsed_directory / "ttl_results.csv")

    _plot_sync(quality, output_directory, outputs)
    _plot_sync_pairs(pairs, output_directory, outputs)
    _plot_predictions(predictions, output_directory, outputs)
    _plot_command_rtt(
        [row for row in commands if row.get("opcode") == "clock_exchange"],
        output_directory,
        outputs,
        title="Background clock exchange RTT",
        name="clock_exchange_rtt.png",
    )
    _plot_command_rtt(
        [row for row in commands if row.get("opcode") != "clock_exchange"],
        output_directory,
        outputs,
        title="Experiment control command RTT",
        name="control_command_rtt.png",
    )
    _plot_command_stages(commands, output_directory, outputs)
    _plot_command_groups(commands, results, output_directory, outputs)
    _plot_records(records, events, output_directory, outputs)
    _plot_health(health, output_directory, outputs)
    _plot_clock_continuity(records, health, output_directory, outputs)
    _plot_ttl(ttl, output_directory, outputs)
    _plot_record_gaps(record_gaps, output_directory, outputs)
    _plot_record_type_matrix(record_types, output_directory, outputs)
    _plot_counter_deltas(counter_deltas, anomalies, output_directory, outputs)
    _plot_host_transport(host_health, counter_deltas,
                         output_directory, outputs)
    _plot_anomaly_overview(anomalies, output_directory, outputs)
    _plot_correlations(correlations, output_directory, outputs)
    _plot_command_deep(commands, output_directory, outputs)
    _plot_sync_distributions(quality, output_directory, outputs)
    _plot_ttl_completeness(ttl, output_directory, outputs)
    _plot_anomaly_zooms(anomalies, timeseries, output_directory, outputs)
    descriptions = {
        "verification_anomaly_overview.png": "All automatically detected anomaly intervals on one timeline",
        "korora_drop_burst_periodicity.png": "Korora outbound drop bursts and their phase against rollover candidates",
        "counter_increment_diagnostics.png": "Counter increments and rates instead of misleading cumulative totals",
        "record_gap_timeline_and_cdf.png": "Per node record silences and their distribution",
        "longest_record_silences.png": "The longest periods with no record from each logical node",
        "host_ble_full_diagnostics.png": "Adelie BLE queue, inactivity, throughput, errors and clock exchange state",
        "verification_metric_correlations.png": "Strongest relationships between aligned one second metrics",
        "record_rate_matrix.png": "Record cadence by logical node and Fairy record type",
        "clock_and_delivery_continuity.png": "Distinguishes a device clock stop or reset from delayed BLE delivery",
        "command_rtt_cdf_and_histogram.png": "Full command latency distribution by opcode",
        "sync_quality_distributions.png": "Per node synchronization RMS and skew distributions",
        "ttl_completeness_and_cdf.png": "TTL pipeline completeness and absolute error distribution",
    }
    index_lines = [
        "# Plot index",
        "",
        f"Generated {len(outputs)} plots.",
        "",
        "Open anomaly zoom plots first, then use the overview plots to determine "
        "whether the same interval also affected BLE, RS485, synchronization, TTL, "
        "or record arrival.",
        "",
        "| Plot | Purpose |",
        "| --- | --- |",
    ]
    for path in sorted(outputs, key=lambda value: value.name):
        description = descriptions.get(path.name)
        if description is None and path.name.startswith("anomaly_"):
            description = "Automatic multi metric zoom around one counter burst"
        if description is None:
            description = path.stem.replace("_", " ").capitalize()
        index_lines.append(f"| `{path.name}` | {description} |")
    (output_directory / "plot_index.md").write_text(
        "\n".join(index_lines) + "\n", encoding="utf-8"
    )
    return outputs


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Plot parsed Adelie data")
    parser.add_argument("parsed_directory", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    for path in create_plots(args.parsed_directory, args.output):
        print(path)


if __name__ == "__main__":
    main()
