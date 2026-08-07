from __future__ import annotations

import argparse
import math
from pathlib import Path

import matplotlib.pyplot as plt
import matplotlib.patheffects as path_effects
import numpy as np
import pandas as pd
from cycler import cycler

REFERENCE_NODE = "korora"

NODE_NAMES = {
    "adelie": "Adelie",
    "fairy": "Fairy",
    "galapagos": "Galapagos",
    "korora": "Korora",
}

_GENERATED_PLOTS: list[Path] = []


# Poster palette: soft enough to feel pastel, dark enough to remain readable
# when printed. The first colours are also used as Matplotlib's automatic
# series colour cycle.
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

SERIES_COLOURS = [
    PALETTE["lavender"],
    PALETTE["mint"],
    PALETTE["orchid"],
    PALETTE["sky"],
    PALETTE["amber"],
    PALETTE["violet"],
]


def apply_poster_theme() -> None:
    """Apply a clean, print-friendly pastel-purple Matplotlib theme."""
    plt.rcParams.update(
        {
            "figure.facecolor": PALETTE["paper"],
            "savefig.facecolor": PALETTE["paper"],
            "axes.facecolor": PALETTE["panel"],
            "axes.edgecolor": PALETTE["grid"],
            "axes.labelcolor": PALETTE["ink"],
            "axes.titlecolor": PALETTE["ink"],
            "axes.titleweight": "bold",
            "axes.titlesize": 17,
            "axes.titlelocation": "left",
            "axes.labelsize": 12.5,
            "axes.prop_cycle": cycler(color=SERIES_COLOURS),
            "xtick.color": PALETTE["muted"],
            "ytick.color": PALETTE["muted"],
            "xtick.labelsize": 10.5,
            "ytick.labelsize": 10.5,
            "font.family": "sans-serif",
            "font.sans-serif": [
                "Aptos",
                "Avenir Next",
                "Inter",
                "DejaVu Sans",
            ],
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


def pretty_node(node: str) -> str:
    """Return a stable, presentation-ready device name."""
    return NODE_NAMES.get(node.casefold(), node.replace("_", " ").strip().title())


def graph_title(node: str, measurement: str) -> str:
    """Build every device-specific title using the same visual grammar."""
    return f"{pretty_node(node)} · {measurement}"


def add_stat_badge(
    axis: plt.Axes,
    text: str,
    *,
    x: float = 0.02,
    y: float = 0.96,
) -> None:
    """Add a compact poster-style statistic badge inside an axis."""
    axis.text(
        x,
        y,
        text,
        transform=axis.transAxes,
        ha="right" if x >= 0.5 else "left",
        va="top",
        color=PALETTE["ink"],
        fontsize=10.5,
        fontweight="semibold",
        bbox={
            "boxstyle": "round,pad=0.42,rounding_size=0.2",
            "facecolor": "#F2EAFB",
            "edgecolor": "none",
            "alpha": 0.96,
        },
        zorder=8,
    )


def add_summary_badge(
    axis: plt.Axes,
    values: pd.Series | np.ndarray | list[float],
    unit: str,
    *,
    signed: bool = False,
    x: float = 0.98,
    y: float = 0.96,
) -> None:
    """Show the same median, P95 and maximum summary on numeric plots."""
    finite = pd.to_numeric(pd.Series(values), errors="coerce").dropna()
    finite = finite[np.isfinite(finite)]
    if finite.empty:
        return

    percentile_values = finite.abs() if signed else finite
    p95_label = "|P95|" if signed else "P95"
    max_label = "|Max|" if signed else "Max"
    add_stat_badge(
        axis,
        (
            f"Median {finite.median():.3g} {unit}  •  "
            f"{p95_label} {percentile_values.quantile(0.95):.3g} {unit}  •  "
            f"{max_label} {percentile_values.max():.3g} {unit}"
        ),
        x=x,
        y=y,
    )


def style_axis(axis: plt.Axes) -> None:
    axis.set_axisbelow(True)
    axis.grid(True, axis="y")
    axis.grid(False, axis="x")
    axis.tick_params(axis="both", which="both", length=0, pad=7)
    axis.margins(x=0.015)
    axis.title.set_path_effects(
        [path_effects.withStroke(linewidth=2.0, foreground=PALETTE["paper"])]
    )


apply_poster_theme()


def load_csv(path: Path) -> pd.DataFrame:
    if not path.exists():
        return pd.DataFrame()
    try:
        return pd.read_csv(path)
    except pd.errors.EmptyDataError:
        return pd.DataFrame()


def safe_name(value: str) -> str:
    return (
        "".join(
            character if character.isalnum() or character in "-_" else "_"
            for character in value
        ).strip("_")
        or "unknown"
    )


def finish(
    figure: plt.Figure,
    axis: plt.Axes,
    path: Path,
    show: bool,
    *,
    bottom_margin: float = 0.0,
) -> None:
    style_axis(axis)

    handles, labels = axis.get_legend_handles_labels()
    if handles:
        legend = axis.legend(
            handles,
            labels,
            loc="best",
            borderpad=0.75,
            handlelength=2.4,
            labelspacing=0.55,
        )
        legend.get_frame().set_linewidth(0.8)

    figure.tight_layout(pad=1.1, rect=(0.0, bottom_margin, 1.0, 1.0))
    figure.savefig(path)

    # SVG is ideal for posters: it stays perfectly sharp at any size and can
    # be edited in Illustrator, Inkscape, Affinity Designer, or PowerPoint.
    figure.savefig(path.with_suffix(".svg"))
    _GENERATED_PLOTS.append(path)

    if show:
        plt.show()
    plt.close(figure)


def write_plot_index(output: Path, plots: list[Path]) -> None:
    """Write a compact, human-readable catalogue beside the plot files."""
    descriptions = {
        "cross_device_event_alignment.png": "Event timing offsets from Korora for every selected remote device",
        "cross_device_event_error_by_node.png": "Cross-device event timing error distributions grouped by device",
        "korora_reference_event_interval.png": "Spacing between consecutive Korora reference events",
        "adelie_clock_sync_round_trip_time.png": "BLE clock synchronization latency excluding Korora processing time",
        "adelie_clock_model_fit.png": "Rolling clock model fit error on Adelie",
        "adelie_clock_rate_estimate.png": "Adelie clock rate error relative to nominal",
        "adelie_clock_prediction_error.png": "One-step Adelie clock prediction error",
        "adelie_command_round_trip_time.png": "End-to-end latency for Adelie commands",
        "adelie_command_latency_by_stage.png": "Median command latency split by transport and processing stage",
        "adelie_ble_direction_latency.png": "Estimated BLE latency in each direction",
        "ttl_timing_components.png": "Generation, acquisition and end-to-end TTL timing components",
        "ttl_timing_error_distribution.png": "Distribution of completed TTL end-to-end timing error",
        "ttl_pulse_outcomes.png": "Counts of completed, timed-out and incomplete TTL pulses",
        "ttl_timing_field_consistency.png": "Reported TTL timing fields checked against recomputed timestamps",
    }
    lines = [
        "# Synchronization plot index",
        "",
        f"Generated {len(plots)} plots. Each plot is available as PNG and SVG.",
        "",
        "| Plot | Purpose |",
        "| --- | --- |",
    ]
    for path in sorted(plots, key=lambda item: item.name):
        description = descriptions.get(
            path.name,
            path.stem.replace("_", " ").capitalize(),
        )
        lines.append(f"| `{path.name}` | {description} |")
    (output / "plot_index.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def add_reset_markers(
    axis: plt.Axes,
    node: str,
    diagnostics: pd.DataFrame,
    pairs: pd.DataFrame,
) -> None:
    if diagnostics.empty or pairs.empty:
        return
    resets = diagnostics[
        (diagnostics["node"] == node) & (
            diagnostics["record_type"] == "MODEL_RESET")
    ]
    node_pairs = pairs[pairs["node"] == node]
    if resets.empty or node_pairs.empty:
        return

    pulse_to_time = (
        node_pairs.drop_duplicates("pulse").set_index("pulse")[
            "elapsed_s"].to_dict()
    )
    first = True
    for pulse in pd.to_numeric(resets["pulse"], errors="coerce").dropna():
        elapsed = pulse_to_time.get(int(pulse))
        if elapsed is None:
            continue
        axis.axvline(
            elapsed,
            color=PALETTE["reset"],
            linestyle=(0, (3, 4)),
            linewidth=1.45,
            alpha=0.82,
            label="Model reset" if first else None,
            zorder=1,
        )
        first = False


def correlation(frame: pd.DataFrame, x: str, y: str) -> float:
    data = frame[[x, y]].dropna()
    if len(data) < 2:
        return math.nan
    if data[x].nunique() < 2 or data[y].nunique() < 2:
        return math.nan
    return float(data.corr().iloc[0, 1])


def plot_clock_rate(
    node: str,
    pairs: pd.DataFrame,
    sync: pd.DataFrame,
    diagnostics: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    node_pairs = (
        pairs[(pairs["node"] == node) & (pairs["has_previous"] == 1)]
        if not pairs.empty
        else pd.DataFrame()
    )
    track = (
        sync[(sync["node"] == node) & (sync["state"] == "TRACK")]
        if not sync.empty
        else pd.DataFrame()
    )
    if node_pairs.empty and track.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    if not node_pairs.empty:
        axis.plot(
            node_pairs["elapsed_s"],
            node_pairs["local_interval_error_ppm"],
            linewidth=1,
            label="Measured anchor interval",
        )
    if not track.empty:
        axis.plot(
            track["elapsed_s"],
            -track["slope_ppm"],
            linewidth=1.4,
            label="Firmware clock model",
        )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(graph_title(node, "Clock rate stability"))
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Rate error (ppm)")
    rate_error = (
        -track["slope_ppm"]
        if not track.empty
        else node_pairs["local_interval_error_ppm"]
    )
    add_summary_badge(axis, rate_error, "ppm", signed=True)
    finish(
        figure, axis, output / f"{safe_name(node)}_clock_rate_stability.png", show
    )


def plot_rms(
    node: str,
    sync: pd.DataFrame,
    rolling: pd.DataFrame,
    diagnostics: pd.DataFrame,
    pairs: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    track = (
        sync[(sync["node"] == node) & (sync["state"] == "TRACK")]
        if not sync.empty
        else pd.DataFrame()
    )
    fits = rolling[rolling["node"] ==
                   node] if not rolling.empty else pd.DataFrame()
    if track.empty and fits.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    if not track.empty:
        axis.plot(
            track["elapsed_s"],
            track["rms_us"],
            linewidth=1.4,
            label="Firmware fit error",
        )
    if not fits.empty:
        axis.plot(
            fits["end_elapsed_s"],
            fits["rms_us"],
            linewidth=1,
            alpha=0.75,
            label="Independent fit error",
        )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(graph_title(node, "Clock model fit"))
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Fit error RMS (µs)")
    axis.set_ylim(bottom=0)
    add_summary_badge(axis, track["rms_us"] if not track.empty else fits["rms_us"], "µs")
    finish(figure, axis, output / f"{safe_name(node)}_clock_model_fit.png", show)


def plot_event_error(
    node: str,
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if events.empty:
        return
    data = events[
        (events["node"] == node) & events["is_gpio_event"] & events["matched"]
    ].dropna(subset=["elapsed_s", "error_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.scatter(
        data["elapsed_s"],
        data["error_us"],
        s=14,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    axis.set_title(graph_title(node, "External event timing error"))
    axis.set_xlabel("Event time (s)")
    axis.set_ylabel("Timing error (µs)")
    add_summary_badge(axis, data["error_us"], "µs", signed=True)
    finish(
        figure, axis, output / f"{safe_name(node)}_event_timing_error.png", show
    )


def plot_tracking_state(
    node: str,
    sync: pd.DataFrame,
    diagnostics: pd.DataFrame,
    pairs: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if sync.empty:
        return
    data = sync[sync["node"] == node].copy()
    if data.empty:
        return
    data["state_value"] = data["state"].map({"ACQUIRE": 0, "TRACK": 1})
    data = data.dropna(subset=["elapsed_s", "state_value"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 3.5))
    axis.step(
        data["elapsed_s"],
        data["state_value"],
        where="post",
        linewidth=2.2,
        color=PALETTE["lavender"],
    )
    axis.fill_between(
        data["elapsed_s"],
        0,
        data["state_value"],
        step="post",
        color=PALETTE["lilac"],
        alpha=0.28,
    )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(graph_title(node, "Clock synchronization state"))
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Synchronization state")
    axis.set_yticks([0, 1])
    axis.set_yticklabels(["Acquire", "Track"])
    axis.set_ylim(-0.15, 1.15)
    finish(figure, axis, output /
           f"{safe_name(node)}_clock_sync_state.png", show)


def plot_prediction_histogram(
    node: str,
    sync: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if sync.empty:
        return
    residual = pd.to_numeric(
        sync.loc[
            (sync["node"] == node) & (sync["state"] == "TRACK"),
            "prefit_residual_us",
        ],
        errors="coerce",
    ).dropna()
    if residual.empty:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    bins = min(60, max(15, int(np.sqrt(len(residual)))))
    axis.hist(
        residual,
        bins=bins,
        color=PALETTE["violet"],
        alpha=0.86,
        edgecolor="white",
        linewidth=0.75,
    )
    abs_p95 = float(np.percentile(np.abs(residual), 95))
    axis.axvline(float(residual.median()), linestyle="--", linewidth=1.2)
    axis.axvline(abs_p95, linestyle=":", linewidth=1.2)
    axis.axvline(-abs_p95, linestyle=":", linewidth=1.2)
    axis.set_title(graph_title(node, "Clock prediction error distribution"))
    axis.set_xlabel("Prediction error (µs)")
    axis.set_ylabel("Samples")
    add_summary_badge(axis, residual, "µs", signed=True)
    finish(figure, axis, output /
           f"{safe_name(node)}_clock_prediction_error_distribution.png", show)


def plot_error_vs_transport(
    node: str,
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    required = {"node", "is_gpio_event",
                "matched", "transport_age_us", "error_us"}
    if events.empty or not required.issubset(events.columns):
        return
    data = events[
        (events["node"] == node) & events["is_gpio_event"] & events["matched"]
    ][["transport_age_us", "error_us"]].dropna()
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    axis.scatter(
        data["transport_age_us"],
        data["error_us"],
        s=16,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    corr = correlation(data, "transport_age_us", "error_us")
    if math.isfinite(corr):
        add_stat_badge(axis, f"Pearson r  {corr:.3f}", x=0.03, y=0.95)
    axis.set_title(graph_title(node, "Event timing error vs. delivery delay"))
    axis.set_xlabel("Event delivery delay (µs)")
    axis.set_ylabel("Timing error (µs)")
    finish(
        figure, axis, output /
        f"{safe_name(node)}_event_error_vs_delivery_delay.png", show
    )


def plot_prediction_error(
    node: str,
    sync: pd.DataFrame,
    diagnostics: pd.DataFrame,
    pairs: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if sync.empty:
        return
    data = sync[(sync["node"] == node) & (sync["state"] == "TRACK")].dropna(
        subset=["elapsed_s", "prefit_residual_us"]
    )
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.plot(
        data["elapsed_s"],
        data["prefit_residual_us"],
        linewidth=1,
        label="Prediction error",
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(graph_title(node, "Clock prediction error"))
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Prediction error (µs)")
    add_summary_badge(axis, data["prefit_residual_us"], "µs", signed=True)
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_clock_prediction_error.png",
        show,
    )


def plot_instability_relationship(
    node: str,
    rolling: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if rolling.empty:
        return
    data = rolling[rolling["node"] == node][
        ["clock_instability_ppm", "rms_us"]
    ].dropna()
    if len(data) < 2:
        return

    x = data["clock_instability_ppm"].to_numpy()
    y = data["rms_us"].to_numpy()
    if np.unique(x).size < 2 or np.unique(y).size < 2:
        return

    corr = float(np.corrcoef(x, y)[0, 1])
    figure, axis = plt.subplots(figsize=(8.5, 5))
    axis.scatter(x, y, alpha=0.55)

    try:
        slope, intercept = np.polyfit(x, y, 1)
    except np.linalg.LinAlgError:
        plt.close(figure)
        return

    x_line = np.linspace(float(np.min(x)), float(np.max(x)), 100)
    axis.plot(
        x_line,
        slope * x_line + intercept,
        linewidth=1.5,
        label="Trend",
    )
    add_stat_badge(axis, f"Pearson r  {corr:.3f}", x=0.03, y=0.95)
    axis.set_title(graph_title(node, "Clock stability vs. model fit"))
    axis.set_xlabel("Short-term clock instability (ppm)")
    axis.set_ylabel("Fit error RMS (µs)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_clock_stability_vs_model_fit.png",
        show,
    )


def plot_model_step(
    node: str,
    sync: pd.DataFrame,
    diagnostics: pd.DataFrame,
    pairs: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if sync.empty:
        return
    data = sync[(sync["node"] == node) & (sync["state"] == "TRACK")].dropna(
        subset=["elapsed_s", "model_step_us"]
    )
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.plot(
        data["elapsed_s"],
        data["model_step_us"],
        linewidth=1,
        label="Model adjustment",
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(graph_title(node, "Clock model adjustment"))
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Adjustment at latest sample (µs)")
    add_summary_badge(axis, data["model_step_us"], "µs", signed=True)
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_clock_model_adjustment.png",
        show,
    )


def plot_transport_age(
    node: str,
    pairs: pd.DataFrame,
    diagnostics: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if pairs.empty:
        return
    data = pairs[pairs["node"] == node].dropna(
        subset=["elapsed_s", "transport_age_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 4.5))
    axis.scatter(
        data["elapsed_s"],
        data["transport_age_us"],
        s=12,
        label="Delivery delay",
    )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(graph_title(node, "Synchronization record delivery delay"))
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Delivery delay (µs)")
    add_summary_badge(axis, data["transport_age_us"], "µs")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_sync_record_delivery_delay.png",
        show,
    )


def plot_report_interval(
    node: str,
    pairs: pd.DataFrame,
    diagnostics: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if pairs.empty or "report_interval_ms" not in pairs:
        return
    data = pairs[(pairs["node"] == node) & pairs["report_interval_ms"].notna()]
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 4.5))
    axis.scatter(
        data["elapsed_s"],
        data["report_interval_ms"],
        s=12,
        label="Received synchronization interval",
    )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(graph_title(node, "Synchronization record interval"))
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Received interval (ms)")
    add_summary_badge(axis, data["report_interval_ms"], "ms")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_sync_record_interval.png",
        show,
    )


def plot_event_error_histogram(
    node: str,
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if events.empty:
        return
    data = pd.to_numeric(
        events.loc[
            (events["node"] == node) & events["is_gpio_event"] & events["matched"],
            "error_us",
        ],
        errors="coerce",
    ).dropna()
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    bins = min(60, max(15, int(np.sqrt(len(data)))))
    axis.hist(
        data,
        bins=bins,
        color=PALETTE["violet"],
        alpha=0.86,
        edgecolor="white",
        linewidth=0.75,
    )
    abs_p95 = float(np.percentile(np.abs(data), 95))
    axis.axvline(float(data.median()), linestyle="--", linewidth=1.2)
    axis.axvline(abs_p95, linestyle=":", linewidth=1.2)
    axis.axvline(-abs_p95, linestyle=":", linewidth=1.2)
    axis.set_title(graph_title(node, "External event timing error distribution"))
    axis.set_xlabel("Timing error (µs)")
    axis.set_ylabel("Events")
    add_summary_badge(axis, data, "µs", signed=True)
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_event_timing_error_distribution.png",
        show,
    )


def plot_event_transport_age(
    node: str,
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if events.empty:
        return
    data = events[(events["node"] == node) & events["is_gpio_event"]].dropna(
        subset=["transport_age_us"]
    )
    if data.empty:
        return

    # Prefer the real event timeline, but never coerce events without a valid
    # hub-domain time onto x=0.  When no event in this node has a usable time,
    # fall back to log position for the whole plot.
    timed = data.dropna(subset=["elapsed_s"])
    if not timed.empty:
        data = timed
        x_column = "elapsed_s"
        x_label = "Event time (s)"
    else:
        x_column = "source_line"
        x_label = "Log line"

    figure, axis = plt.subplots(figsize=(11, 4.5))
    axis.scatter(
        data[x_column],
        data["transport_age_us"],
        s=14,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    axis.set_title(graph_title(node, "External event delivery delay"))
    axis.set_xlabel(x_label)
    axis.set_ylabel("Delivery delay (µs)")
    add_summary_badge(axis, data["transport_age_us"], "µs")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_event_delivery_delay.png",
        show,
    )


def plot_event_error_plus_transport(
    node: str,
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if events.empty:
        return
    data = events[
        (events["node"] == node) & events["is_gpio_event"] & events["matched"]
    ].dropna(subset=["elapsed_s", "error_plus_transport_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.scatter(
        data["elapsed_s"],
        data["error_plus_transport_us"],
        s=14,
        label="Timing error + delivery delay",
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    axis.set_title(graph_title(node, "Received event timing offset"))
    axis.set_xlabel("Event time (s)")
    axis.set_ylabel("Timing error + delivery delay (µs)")
    add_summary_badge(axis, data["error_plus_transport_us"], "µs", signed=True)
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_received_event_timing_offset.png",
        show,
    )


def plot_event_state(
    node: str,
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if events.empty:
        return
    data = events[(events["node"] == node) & events["is_gpio_event"]].copy()
    if data.empty:
        return

    data["state_value"] = data["state"].map(
        {"UNSYNC": 0, "ACQUIRE": 0, "REMOTE": 0, "TRACK": 1, "LOCAL": 1}
    )
    data = data.dropna(subset=["source_line", "state_value"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 3.5))
    axis.step(
        data["source_line"],
        data["state_value"],
        where="post",
        linewidth=2.2,
        color=PALETTE["lavender"],
    )
    axis.fill_between(
        data["source_line"],
        0,
        data["state_value"],
        step="post",
        color=PALETTE["lilac"],
        alpha=0.28,
    )
    axis.set_title(graph_title(node, "External event synchronization state"))
    axis.set_xlabel("Log line")
    axis.set_ylabel("State")
    axis.set_yticks([0, 1])
    axis.set_yticklabels(["Unsynchronised", "Converted"])
    axis.set_ylim(-0.15, 1.15)
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_event_sync_state.png",
        show,
    )


def plot_event_alignment(
    events: pd.DataFrame,
    output: Path,
    show: bool,
    selected_nodes: set[str],
) -> None:
    if events.empty:
        return
    data = events[
        events["is_gpio_event"]
        & events["matched"]
        & (events["node"] != REFERENCE_NODE)
        & events["reference_event_id"].notna()
        & (events["reference_event_id"] > 0)
        & events["error_us"].notna()
        & events["node"].isin(selected_nodes)
    ].copy()
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    for node, node_data in data.groupby("node"):
        axis.scatter(
            node_data["reference_event_id"],
            node_data["error_us"],
            s=24,
            alpha=0.74,
            edgecolors="white",
            linewidths=0.45,
            label=pretty_node(str(node)),
        )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    axis.set_title("Cross-device event alignment")
    axis.set_xlabel("Matched Korora event number")
    axis.set_ylabel("Timing offset from Korora (µs)")
    add_summary_badge(axis, data["error_us"], "µs", signed=True)
    finish(figure, axis, output / "cross_device_event_alignment.png", show)


def plot_event_error_distribution(
    events: pd.DataFrame,
    output: Path,
    show: bool,
    selected_nodes: set[str],
) -> None:
    if events.empty:
        return
    groups: list[np.ndarray] = []
    labels: list[str] = []
    summary_rows: list[list[str]] = []
    for node in sorted(selected_nodes - {REFERENCE_NODE, "adelie"}):
        values = pd.to_numeric(
            events.loc[
                (events["node"] ==
                 node) & events["is_gpio_event"] & events["matched"],
                "error_us",
            ],
            errors="coerce",
        ).dropna()
        if not values.empty:
            samples = values.to_numpy()
            absolute_samples = np.abs(samples)
            groups.append(samples)
            labels.append(node)
            summary_rows.append(
                [
                    f"{len(samples):,}",
                    f"{np.median(samples):.3g}",
                    f"{np.percentile(absolute_samples, 95):.3g}",
                    f"{np.max(absolute_samples):.3g}",
                ]
            )
    if not groups:
        return

    figure, axis = plt.subplots(figsize=(max(8.5, 1.4 * len(groups) + 5.5), 6.2))
    axis.boxplot(
        groups,
        tick_labels=[pretty_node(label) for label in labels],
        showfliers=True,
        patch_artist=True,
        boxprops={"facecolor": PALETTE["lilac"], "alpha": 0.78},
        medianprops={"color": PALETTE["ink"], "linewidth": 2.0},
        whiskerprops={"color": PALETTE["lavender"], "linewidth": 1.4},
        capprops={"color": PALETTE["lavender"], "linewidth": 1.4},
        flierprops={
            "marker": "o",
            "markerfacecolor": PALETTE["orchid"],
            "markeredgecolor": "white",
            "markersize": 4.5,
            "alpha": 0.65,
        },
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    axis.set_title("Cross-device event timing error by node")
    axis.set_ylabel("Timing error (µs)")
    table = axis.table(
        cellText=summary_rows,
        rowLabels=[pretty_node(label) for label in labels],
        colLabels=["n", "Median (µs)", "|P95| (µs)", "|Max| (µs)"],
        cellLoc="center",
        rowLoc="center",
        bbox=[0.08, -0.42, 0.9, 0.26],
    )
    table.auto_set_font_size(False)
    table.set_fontsize(9.5)
    for (row, column), cell in table.get_celld().items():
        cell.set_edgecolor(PALETTE["grid"])
        cell.set_linewidth(0.8)
        cell.set_text_props(color=PALETTE["ink"])
        if row == 0:
            cell.set_facecolor("#F2EAFB")
            cell.set_text_props(weight="semibold")
        elif column == -1:
            cell.set_facecolor(PALETTE["paper"])
            cell.set_text_props(weight="semibold")
        else:
            cell.set_facecolor(PALETTE["panel"])
    finish(
        figure,
        axis,
        output / "cross_device_event_error_by_node.png",
        show,
        bottom_margin=0.23,
    )


def plot_reference_event_spacing(
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if events.empty:
        return
    data = events[
        (events["node"] == REFERENCE_NODE)
        & events["is_gpio_event"]
        & events["hub_ticks"].notna()
    ].sort_values("hub_ticks")
    if len(data) < 2:
        return

    spacing_ms = data["hub_ticks"].diff().dropna() / 16_000_000.0 * 1000.0
    figure, axis = plt.subplots(figsize=(10, 4.5))
    axis.scatter(
        np.arange(1, len(spacing_ms) + 1),
        spacing_ms,
        s=14,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    axis.set_title("Korora · Reference event interval")
    axis.set_xlabel("Reference event interval number")
    axis.set_ylabel("Interval (ms)")
    add_summary_badge(axis, spacing_ms, "ms")
    finish(figure, axis, output / "korora_reference_event_interval.png", show)


def plot_adelie_network_rtt(
    clock: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if clock.empty:
        return
    data = clock.dropna(subset=["elapsed_s", "network_rtt_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.scatter(
        data["elapsed_s"],
        data["network_rtt_us"],
        s=14,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    axis.set_title("Adelie · BLE clock sync round-trip time")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Round-trip time excluding Korora processing (µs)")
    add_summary_badge(axis, data["network_rtt_us"], "µs")
    finish(figure, axis, output / "adelie_clock_sync_round_trip_time.png", show)


def plot_adelie_model_rms(
    rolling: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if rolling.empty:
        return
    data = rolling.dropna(subset=["end_elapsed_s", "rms_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.plot(data["end_elapsed_s"], data["rms_us"], linewidth=1.3)
    axis.set_title("Adelie · Clock model fit")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Fit error RMS (µs)")
    axis.set_ylim(bottom=0)
    add_summary_badge(axis, data["rms_us"], "µs")
    finish(figure, axis, output / "adelie_clock_model_fit.png", show)


def plot_adelie_slope_error(
    rolling: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if rolling.empty:
        return
    data = rolling.dropna(subset=["end_elapsed_s", "slope_error_ppm"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.plot(data["end_elapsed_s"], data["slope_error_ppm"], linewidth=1.2)
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    axis.set_title("Adelie · Clock rate estimate")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Rate error (ppm)")
    add_summary_badge(axis, data["slope_error_ppm"], "ppm", signed=True)
    finish(figure, axis, output / "adelie_clock_rate_estimate.png", show)


def plot_adelie_prediction_error(
    rolling: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if rolling.empty:
        return
    data = rolling.dropna(subset=["end_elapsed_s", "prediction_error_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.scatter(
        data["end_elapsed_s"],
        data["prediction_error_us"],
        s=14,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    axis.set_title("Adelie · Clock prediction error")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Prediction error (µs)")
    add_summary_badge(axis, data["prediction_error_us"], "µs", signed=True)
    finish(figure, axis, output / "adelie_clock_prediction_error.png", show)


def plot_adelie_command_rtt(
    commands: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if commands.empty:
        return
    data = commands.dropna(subset=["sequence", "total_rtt_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(10, 5))
    axis.scatter(
        data["sequence"],
        data["total_rtt_us"],
        s=24,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    axis.set_title("Adelie · Command round-trip time")
    axis.set_xlabel("Command number")
    axis.set_ylabel("End-to-end latency (µs)")
    add_summary_badge(axis, data["total_rtt_us"], "µs")
    finish(figure, axis, output / "adelie_command_round_trip_time.png", show)


def plot_adelie_command_breakdown(
    commands: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if commands.empty:
        return

    stages = [
        ("BLE to Korora", "ble_down_est_us"),
        ("Korora command queue", "korora_queue_to_i2c_us"),
        ("I²C to Fairy", "i2c_down_us"),
        ("Fairy processing", "fairy_action_us"),
        ("I²C response", "i2c_up_and_poll_us"),
        ("Korora result handling", "korora_post_ack_us"),
        ("BLE to Adelie", "ble_up_est_us"),
    ]
    labels: list[str] = []
    medians: list[float] = []
    for label, column in stages:
        if column not in commands:
            continue
        values = pd.to_numeric(commands[column], errors="coerce").dropna()
        if values.empty:
            continue
        labels.append(label)
        medians.append(float(values.median()))
    if not medians:
        return

    figure, axis = plt.subplots(figsize=(10, 5.5))
    positions = np.arange(len(labels))
    bars = axis.bar(
        positions,
        medians,
        color=[SERIES_COLOURS[index % len(SERIES_COLOURS)]
               for index in positions],
        edgecolor="white",
        linewidth=0.8,
    )
    axis.bar_label(
        bars,
        labels=[f"{value:.0f}" for value in medians],
        padding=4,
        color=PALETTE["ink"],
        fontsize=9.5,
        fontweight="semibold",
    )
    axis.set_xticks(positions)
    axis.set_xticklabels(labels, rotation=30, ha="right")
    axis.set_title("Adelie · Median command latency by stage")
    axis.set_xlabel("Command stage")
    axis.set_ylabel("Median latency (µs)")
    finish(figure, axis, output / "adelie_command_latency_by_stage.png", show)


def plot_adelie_ble_directions(
    commands: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    data = commands[["ble_down_est_us", "ble_up_est_us"]].dropna()
    if len(data) < 2:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    axis.scatter(
        data["ble_down_est_us"],
        data["ble_up_est_us"],
        s=22,
        alpha=0.72,
        edgecolors="white",
        linewidths=0.45,
    )
    lower = float(min(data.min()))
    upper = float(max(data.max()))
    if upper > lower:
        guide = np.linspace(lower, upper, 100)
        axis.plot(guide, guide, linestyle="--",
                  linewidth=1, label="Equal latency")
    axis.set_title("Adelie · BLE direction latency comparison")
    axis.set_xlabel("Adelie → Korora latency (µs)")
    axis.set_ylabel("Korora → Adelie latency (µs)")
    finish(figure, axis, output / "adelie_ble_direction_latency.png", show)


def plot_ttl_error_components(
    ttl_pulses: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if ttl_pulses.empty:
        return
    data = ttl_pulses[ttl_pulses["completed"]].dropna(
        subset=["elapsed_s", "generation_error_us",
                "wire_offset_us", "total_error_us"]
    )
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    axis.scatter(
        data["elapsed_s"],
        data["generation_error_us"],
        s=22,
        alpha=0.75,
        edgecolors="white",
        linewidths=0.45,
        label="Korora generation error",
    )
    axis.scatter(
        data["elapsed_s"],
        data["wire_offset_us"],
        s=22,
        alpha=0.75,
        edgecolors="white",
        linewidths=0.45,
        label="Acquisition offset",
    )
    axis.scatter(
        data["elapsed_s"],
        data["total_error_us"],
        s=28,
        alpha=0.82,
        edgecolors="white",
        linewidths=0.5,
        label="End-to-end timing error",
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    add_summary_badge(axis, data["total_error_us"], "µs", signed=True)
    axis.set_title("Scheduled TTL · Timing components")
    axis.set_xlabel("Scheduled time (s)")
    axis.set_ylabel("Timing error or offset (µs)")
    finish(figure, axis, output / "ttl_timing_components.png", show)


def plot_ttl_total_error_histogram(
    ttl_pulses: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if ttl_pulses.empty:
        return
    values = pd.to_numeric(
        ttl_pulses.loc[ttl_pulses["completed"], "total_error_us"],
        errors="coerce",
    ).dropna()
    if values.empty:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    bins = min(60, max(10, int(np.sqrt(len(values)))))
    axis.hist(
        values,
        bins=bins,
        color=PALETTE["violet"],
        alpha=0.86,
        edgecolor="white",
        linewidth=0.75,
    )
    abs_p95 = float(values.abs().quantile(0.95))
    axis.axvline(float(values.median()), linestyle="--", linewidth=1.2)
    axis.axvline(abs_p95, linestyle=":", linewidth=1.2)
    axis.axvline(-abs_p95, linestyle=":", linewidth=1.2)
    axis.set_title("Scheduled TTL · End-to-end timing error")
    axis.set_xlabel("Acquired time − scheduled time (µs)")
    axis.set_ylabel("Pulses")
    add_summary_badge(axis, values, "µs", signed=True)
    finish(figure, axis, output / "ttl_timing_error_distribution.png", show)


def plot_ttl_outcomes(
    ttl_pulses: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if ttl_pulses.empty:
        return
    counts = ttl_pulses["outcome"].value_counts().reindex(
        ["RESULT", "TIMEOUT", "INCOMPLETE"], fill_value=0
    )
    if int(counts.sum()) == 0:
        return

    figure, axis = plt.subplots(figsize=(8.5, 4.8))
    positions = np.arange(len(counts))
    bars = axis.bar(
        positions,
        counts.to_numpy(),
        color=[SERIES_COLOURS[index % len(SERIES_COLOURS)]
               for index in positions],
        edgecolor="white",
        linewidth=0.8,
    )
    axis.bar_label(
        bars,
        labels=[str(int(value)) for value in counts.to_numpy()],
        padding=4,
        color=PALETTE["ink"],
        fontsize=10,
        fontweight="semibold",
    )
    axis.set_xticks(positions)
    axis.set_xticklabels(["Result", "Timeout", "Incomplete"])
    axis.set_title("Scheduled TTL · Pulse outcomes")
    axis.set_xlabel("Outcome")
    axis.set_ylabel("Pulses")
    finish(figure, axis, output / "ttl_pulse_outcomes.png", show)


def plot_ttl_error_consistency(
    ttl_pulses: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if ttl_pulses.empty:
        return
    data = ttl_pulses[ttl_pulses["completed"]].dropna(
        subset=[
            "elapsed_s",
            "generation_error_consistency_ns",
            "wire_offset_consistency_ns",
            "total_error_consistency_ns",
        ]
    )
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 4.8))
    axis.plot(
        data["elapsed_s"],
        data["generation_error_consistency_ns"],
        linewidth=1.1,
        label="Generation error",
    )
    axis.plot(
        data["elapsed_s"],
        data["wire_offset_consistency_ns"],
        linewidth=1.1,
        label="Acquisition offset",
    )
    axis.plot(
        data["elapsed_s"],
        data["total_error_consistency_ns"],
        linewidth=1.1,
        label="End-to-end error",
    )
    axis.axhline(0, color=PALETTE["muted"],
                 linewidth=1.15, alpha=0.72, zorder=1)
    axis.set_title("Scheduled TTL · Timing field consistency")
    axis.set_xlabel("Scheduled time (s)")
    axis.set_ylabel("Reported minus recomputed (ns)")
    finish(figure, axis, output / "ttl_timing_field_consistency.png", show)


def generate_plots(
    *,
    parsed_dir: Path,
    analysis_dir: Path,
    output_dir: Path,
    node_selection: str,
    profile: str,
    show: bool,
) -> list[Path]:
    output_dir.mkdir(parents=True, exist_ok=True)
    _GENERATED_PLOTS.clear()

    pairs = load_csv(analysis_dir / "pairs_derived.csv")
    sync = load_csv(analysis_dir / "sync_derived.csv")
    rolling = load_csv(analysis_dir / "rolling_fits.csv")
    events_path = analysis_dir / "events_derived.csv"
    if not events_path.exists():
        events_path = analysis_dir / "external_events_derived.csv"
    events = load_csv(events_path)
    diagnostics = load_csv(parsed_dir / "diagnostics.csv")
    adelie_clock = load_csv(analysis_dir / "adelie_clock_derived.csv")
    adelie_rolling = load_csv(analysis_dir / "adelie_rolling_fits.csv")
    adelie_commands = load_csv(analysis_dir / "adelie_commands_derived.csv")
    ttl_pulses = load_csv(analysis_dir / "ttl_pulses_derived.csv")

    available_nodes: set[str] = set()
    for frame in (pairs, sync, events):
        if not frame.empty and "node" in frame:
            available_nodes |= set(frame["node"].dropna().astype(str))
    if not adelie_clock.empty or not adelie_commands.empty:
        available_nodes.add("adelie")

    if not available_nodes:
        raise ValueError("No analysed node or Adelie data found")

    if node_selection == "all":
        selected = set(available_nodes)
    else:
        selected = {item.strip()
                    for item in node_selection.split(",") if item.strip()}
        missing = selected - available_nodes
        if missing:
            raise ValueError(
                f"Nodes not found: {sorted(missing)}. Available: {sorted(available_nodes)}"
            )

    remote_nodes = sorted(
        node for node in selected if node not in {REFERENCE_NODE, "adelie"}
    )

    for node in remote_nodes:
        # Core plots are useful in every profile.
        plot_clock_rate(node, pairs, sync, diagnostics, output_dir, show)
        plot_rms(node, sync, rolling, diagnostics, pairs, output_dir, show)
        plot_event_error(node, events, output_dir, show)

        if profile in {"expanded", "full"}:
            # Restore the original engineering plot set for remote nodes only.
            # Korora remains available as the reference in combined plots, but
            # does not get meaningless zero-error or identity-model plots.
            plot_tracking_state(node, sync, diagnostics,
                                pairs, output_dir, show)
            plot_prediction_error(node, sync, diagnostics,
                                  pairs, output_dir, show)
            plot_prediction_histogram(node, sync, output_dir, show)
            plot_instability_relationship(node, rolling, output_dir, show)
            plot_model_step(node, sync, diagnostics, pairs, output_dir, show)
            plot_transport_age(node, pairs, diagnostics, output_dir, show)
            plot_report_interval(node, pairs, diagnostics, output_dir, show)
            plot_event_state(node, events, output_dir, show)
            plot_event_error_histogram(node, events, output_dir, show)
            plot_event_transport_age(node, events, output_dir, show)
            plot_error_vs_transport(node, events, output_dir, show)
            plot_event_error_plus_transport(node, events, output_dir, show)
        else:
            node_sync = sync[sync["node"] ==
                             node] if not sync.empty else pd.DataFrame()
            node_diagnostics = (
                diagnostics[diagnostics["node"] == node]
                if not diagnostics.empty
                else pd.DataFrame()
            )
            track_fraction = (
                float((node_sync["state"] == "TRACK").mean())
                if not node_sync.empty
                else 1.0
            )
            if track_fraction < 0.95 or (
                not node_diagnostics.empty
                and (node_diagnostics["record_type"] == "MODEL_RESET").any()
            ):
                plot_tracking_state(node, sync, diagnostics,
                                    pairs, output_dir, show)

            data = (
                events[
                    (events["node"] == node)
                    & events["is_gpio_event"]
                    & events["matched"]
                ]
                if not events.empty
                else pd.DataFrame()
            )
            corr = (
                correlation(data, "transport_age_us", "error_us")
                if not data.empty
                else math.nan
            )
            if math.isfinite(corr) and abs(corr) >= 0.25:
                plot_error_vs_transport(node, events, output_dir, show)

    non_reference_selected = selected - {REFERENCE_NODE, "adelie"}
    if non_reference_selected:
        plot_event_alignment(events, output_dir, show, non_reference_selected)
        plot_event_error_distribution(
            events, output_dir, show, non_reference_selected)

    if REFERENCE_NODE in selected and profile == "full":
        plot_reference_event_spacing(events, output_dir, show)

    # TTL is a Galapagos to Korora path, not a pseudo node.  Include its plots
    # in the same report whenever the full data set is requested, or whenever
    # either endpoint is explicitly selected.
    include_ttl = (
        not ttl_pulses.empty
        and (node_selection == "all" or bool(selected & {"galapagos", "korora"}))
    )
    if include_ttl:
        plot_ttl_error_components(ttl_pulses, output_dir, show)
        plot_ttl_total_error_histogram(ttl_pulses, output_dir, show)
        if profile in {"expanded", "full"}:
            plot_ttl_outcomes(ttl_pulses, output_dir, show)
        if profile == "full":
            plot_ttl_error_consistency(ttl_pulses, output_dir, show)

    if "adelie" in selected:
        plot_adelie_network_rtt(adelie_clock, output_dir, show)
        plot_adelie_model_rms(adelie_rolling, output_dir, show)
        plot_adelie_slope_error(adelie_rolling, output_dir, show)
        plot_adelie_prediction_error(adelie_rolling, output_dir, show)
        plot_adelie_command_breakdown(adelie_commands, output_dir, show)
        if len(adelie_commands) >= 2 or profile == "full":
            plot_adelie_command_rtt(adelie_commands, output_dir, show)
        if profile == "full":
            plot_adelie_ble_directions(adelie_commands, output_dir, show)

    generated = sorted(set(_GENERATED_PLOTS))
    write_plot_index(output_dir, generated)
    return generated


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Create poster-ready synchronization, command-latency, and TTL "
            "timing plots as PNG and SVG."
        )
    )
    parser.add_argument("--parsed", type=Path, default=Path("parsed"))
    parser.add_argument("--analysis", type=Path, default=Path("analysis"))
    parser.add_argument("--output", type=Path, default=Path("plots"))
    parser.add_argument(
        "--node",
        default="all",
        help=(
            "all or a comma-separated node selection such as "
            "fairy,galapagos,korora,adelie. TTL path plots are integrated "
            "when all data or either endpoint is selected"
        ),
    )
    parser.add_argument(
        "--profile",
        choices=["curated", "expanded", "full"],
        default="expanded",
        help=(
            "curated generates the core report; expanded adds detailed remote-device "
            "diagnostics; full also includes Korora reference diagnostics"
        ),
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="display plots after saving the PNG and SVG files",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    try:
        created = generate_plots(
            parsed_dir=args.parsed,
            analysis_dir=args.analysis,
            output_dir=args.output,
            node_selection=args.node,
            profile=args.profile,
            show=args.show,
        )
    except ValueError as error:
        raise SystemExit(str(error)) from error

    print(f"Synchronization plots written to: {args.output} (PNG + SVG)")
    print(f"Plot files: {len(created)}")


if __name__ == "__main__":
    main()
