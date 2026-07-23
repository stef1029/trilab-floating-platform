from __future__ import annotations

import argparse
import math
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

REFERENCE_NODE = "korora"


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
) -> None:
    axis.grid(True, alpha=0.3)
    handles, _ = axis.get_legend_handles_labels()
    if handles:
        axis.legend()
    figure.tight_layout()
    figure.savefig(path, dpi=170)
    if show:
        plt.show()
    plt.close(figure)


def add_reset_markers(
    axis: plt.Axes,
    node: str,
    diagnostics: pd.DataFrame,
    pairs: pd.DataFrame,
) -> None:
    if diagnostics.empty or pairs.empty:
        return
    resets = diagnostics[
        (diagnostics["node"] == node) & (diagnostics["record_type"] == "MODEL_RESET")
    ]
    node_pairs = pairs[pairs["node"] == node]
    if resets.empty or node_pairs.empty:
        return

    pulse_to_time = (
        node_pairs.drop_duplicates("pulse").set_index("pulse")["elapsed_s"].to_dict()
    )
    first = True
    for pulse in pd.to_numeric(resets["pulse"], errors="coerce").dropna():
        elapsed = pulse_to_time.get(int(pulse))
        if elapsed is None:
            continue
        axis.axvline(
            elapsed,
            linestyle="--",
            linewidth=1,
            alpha=0.6,
            label="Model reset" if first else None,
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
            label="Measured interval error",
        )
    if not track.empty:
        axis.plot(
            track["elapsed_s"],
            -track["slope_ppm"],
            linewidth=1.4,
            label="Model-estimated clock error",
        )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(f"{node}: relative clock-rate variation")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Clock error (ppm)")
    finish(figure, axis, output / f"{safe_name(node)}_clock_rate.png", show)


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
    fits = rolling[rolling["node"] == node] if not rolling.empty else pd.DataFrame()
    if track.empty and fits.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 5))
    if not track.empty:
        axis.plot(
            track["elapsed_s"],
            track["rms_us"],
            linewidth=1.4,
            label="Firmware RMS",
        )
        median = float(track["rms_us"].median())
        p95 = float(track["rms_us"].quantile(0.95))
        axis.axhline(
            median, linestyle="--", linewidth=1, label=f"Median: {median:.2f} us"
        )
        axis.axhline(p95, linestyle=":", linewidth=1, label=f"P95: {p95:.2f} us")
    if not fits.empty:
        axis.plot(
            fits["end_elapsed_s"],
            fits["rms_us"],
            linewidth=1,
            alpha=0.75,
            label="Independent Python RMS",
        )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(f"{node}: affine model fit error")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("RMS residual (us)")
    axis.set_ylim(bottom=0)
    finish(figure, axis, output / f"{safe_name(node)}_rms.png", show)


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
    axis.scatter(data["elapsed_s"], data["error_us"], s=14)
    axis.axhline(0, linewidth=1)
    median = float(data["error_us"].median())
    abs_p95 = float(data["abs_error_us"].quantile(0.95))
    axis.axhline(median, linestyle="--", linewidth=1, label=f"Median: {median:.2f} us")
    axis.set_title(f"{node}: converted external-event error")
    axis.set_xlabel("Elapsed event time (s)")
    axis.set_ylabel("Timestamp error (us)")
    axis.text(
        0.02,
        0.96,
        f"P95 absolute = {abs_p95:.2f} us",
        transform=axis.transAxes,
        verticalalignment="top",
    )
    finish(figure, axis, output / f"{safe_name(node)}_event_error.png", show)


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
    axis.step(data["elapsed_s"], data["state_value"], where="post", linewidth=1.5)
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(f"{node}: clock-model availability")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("State")
    axis.set_yticks([0, 1])
    axis.set_yticklabels(["Acquire", "Track"])
    axis.set_ylim(-0.15, 1.15)
    finish(figure, axis, output / f"{safe_name(node)}_tracking_state.png", show)


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
    axis.hist(residual, bins=bins, alpha=0.85)
    median = float(residual.median())
    abs_p95 = float(np.percentile(np.abs(residual), 95))
    axis.axvline(
        median, linestyle="--", linewidth=1.2, label=f"Median: {median:.2f} us"
    )
    axis.axvline(
        abs_p95, linestyle=":", linewidth=1.2, label=f"+/-P95: {abs_p95:.2f} us"
    )
    axis.axvline(-abs_p95, linestyle=":", linewidth=1.2)
    axis.set_title(f"{node}: one-step prediction-error distribution")
    axis.set_xlabel("Pre-fit residual (us)")
    axis.set_ylabel("Observations")
    finish(figure, axis, output / f"{safe_name(node)}_prediction_histogram.png", show)


def plot_error_vs_transport(
    node: str,
    events: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    data = events[
        (events["node"] == node) & events["is_gpio_event"] & events["matched"]
    ][["transport_age_us", "error_us"]].dropna()
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    axis.scatter(data["transport_age_us"], data["error_us"], s=16)
    axis.axhline(0, linewidth=1)
    corr = correlation(data, "transport_age_us", "error_us")
    if math.isfinite(corr):
        axis.text(
            0.03,
            0.95,
            f"Pearson r = {corr:.3f}",
            transform=axis.transAxes,
            verticalalignment="top",
        )
    axis.set_title(f"{node}: event error versus transport age")
    axis.set_xlabel("Transport age (us)")
    axis.set_ylabel("Timestamp error (us)")
    finish(
        figure, axis, output / f"{safe_name(node)}_event_error_vs_transport.png", show
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
        label="One-step prediction error",
    )
    axis.axhline(0, linewidth=1)
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(f"{node}: one-step prediction error")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Pre-fit residual (us)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_prediction_error.png",
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
        label="Linear trend",
    )
    axis.text(
        0.03,
        0.95,
        f"Pearson r = {corr:.3f}",
        transform=axis.transAxes,
        verticalalignment="top",
    )
    axis.set_title(f"{node}: model error versus short-term clock instability")
    axis.set_xlabel("Interval-rate standard deviation within window (ppm)")
    axis.set_ylabel("Affine-fit RMS residual (us)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_instability_vs_rms.png",
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
        label="Candidate model difference",
    )
    axis.axhline(0, linewidth=1)
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(f"{node}: rolling model update difference")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Model step at newest point (us)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_model_step.png",
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
    data = pairs[pairs["node"] == node].dropna(subset=["elapsed_s", "transport_age_us"])
    if data.empty:
        return

    figure, axis = plt.subplots(figsize=(11, 4.5))
    axis.scatter(
        data["elapsed_s"],
        data["transport_age_us"],
        s=12,
        label="Transport age",
    )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(f"{node}: synchronization timestamp transport age")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Age (us)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_sync_transport_age.png",
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
        label="Interval between received anchor pairs",
    )
    add_reset_markers(axis, node, diagnostics, pairs)
    axis.set_title(f"{node}: anchor report spacing")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Report interval (ms)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_report_interval.png",
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
    axis.hist(data, bins=bins, alpha=0.85)
    median = float(data.median())
    abs_p95 = float(np.percentile(np.abs(data), 95))
    axis.axvline(
        median,
        linestyle="--",
        linewidth=1.2,
        label=f"Median: {median:.2f} us",
    )
    axis.axvline(
        abs_p95,
        linestyle=":",
        linewidth=1.2,
        label=f"+/-P95 absolute: {abs_p95:.2f} us",
    )
    axis.axvline(-abs_p95, linestyle=":", linewidth=1.2)
    axis.set_title(f"{node}: external-event error distribution")
    axis.set_xlabel("Converted timestamp error (us)")
    axis.set_ylabel("Events")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_event_error_histogram.png",
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
        x_label = "Elapsed event time (s)"
    else:
        x_column = "source_line"
        x_label = "Log line"

    figure, axis = plt.subplots(figsize=(11, 4.5))
    axis.scatter(data[x_column], data["transport_age_us"], s=14)
    axis.set_title(f"{node}: external-event transport age")
    axis.set_xlabel(x_label)
    axis.set_ylabel("Transport age (us)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_event_transport_age.png",
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
        label="error + transport age",
    )
    axis.axhline(0, linewidth=1)
    axis.set_title(f"{node}: event error after adding transport-age diagnostic")
    axis.set_xlabel("Elapsed event time (s)")
    axis.set_ylabel("error + transport age (us)")
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_event_error_plus_transport.png",
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
        linewidth=1.4,
    )
    axis.set_title(f"{node}: external-event conversion availability")
    axis.set_xlabel("Log line")
    axis.set_ylabel("State")
    axis.set_yticks([0, 1])
    axis.set_yticklabels(["Unsynchronised", "Converted"])
    axis.set_ylim(-0.15, 1.15)
    finish(
        figure,
        axis,
        output / f"{safe_name(node)}_event_state.png",
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
            s=14,
            label=str(node),
        )
    axis.axhline(0, linewidth=1)
    axis.set_title("External-event alignment relative to Korora")
    axis.set_xlabel("Matched Korora event ID")
    axis.set_ylabel("Converted timestamp error (us)")
    finish(figure, axis, output / "remote_event_alignment.png", show)


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
    for node in sorted(selected_nodes - {REFERENCE_NODE, "adelie"}):
        values = pd.to_numeric(
            events.loc[
                (events["node"] == node) & events["is_gpio_event"] & events["matched"],
                "error_us",
            ],
            errors="coerce",
        ).dropna()
        if not values.empty:
            groups.append(values.to_numpy())
            labels.append(node)
    if not groups:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    axis.boxplot(groups, tick_labels=labels, showfliers=True)
    axis.axhline(0, linewidth=1)
    axis.set_title("External-event error distribution by remote node")
    axis.set_xlabel("Node")
    axis.set_ylabel("Timestamp error (us)")
    finish(figure, axis, output / "remote_event_error_distribution.png", show)


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
    axis.scatter(np.arange(1, len(spacing_ms) + 1), spacing_ms, s=14)
    axis.set_title("Korora reference-event spacing")
    axis.set_xlabel("Reference-event interval index")
    axis.set_ylabel("Interval (ms)")
    finish(figure, axis, output / "korora_reference_event_spacing.png", show)


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
    axis.scatter(data["elapsed_s"], data["network_rtt_us"], s=14)
    median = float(data["network_rtt_us"].median())
    p95 = float(data["network_rtt_us"].quantile(0.95))
    axis.axhline(median, linestyle="--", linewidth=1, label=f"Median: {median:.1f} us")
    axis.axhline(p95, linestyle=":", linewidth=1, label=f"P95: {p95:.1f} us")
    axis.set_title("Adelie: NTP-style BLE clock-exchange RTT")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Network RTT excluding Korora processing (us)")
    finish(figure, axis, output / "adelie_clock_network_rtt.png", show)


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
    median = float(data["rms_us"].median())
    p95 = float(data["rms_us"].quantile(0.95))
    axis.axhline(median, linestyle="--", linewidth=1, label=f"Median: {median:.1f} us")
    axis.axhline(p95, linestyle=":", linewidth=1, label=f"P95: {p95:.1f} us")
    axis.set_title("Adelie: rolling 16-point affine-model RMS")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("RMS residual (us)")
    axis.set_ylim(bottom=0)
    finish(figure, axis, output / "adelie_clock_model_rms.png", show)


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
    axis.axhline(0, linewidth=1)
    axis.set_title("Adelie: rolling clock-rate estimate")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Slope error from nominal (ppm)")
    finish(figure, axis, output / "adelie_clock_slope_error.png", show)


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
    axis.scatter(data["end_elapsed_s"], data["prediction_error_us"], s=14)
    axis.axhline(0, linewidth=1)
    abs_p95 = float(data["prediction_error_us"].abs().quantile(0.95))
    axis.text(
        0.02,
        0.96,
        f"P95 absolute = {abs_p95:.1f} us",
        transform=axis.transAxes,
        verticalalignment="top",
    )
    axis.set_title("Adelie: one-step clock-model prediction error")
    axis.set_xlabel("Elapsed time (s)")
    axis.set_ylabel("Prediction error (us)")
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
    axis.scatter(data["sequence"], data["total_rtt_us"], s=24)
    median = float(data["total_rtt_us"].median())
    axis.axhline(median, linestyle="--", linewidth=1, label=f"Median: {median:.1f} us")
    axis.set_title("Adelie command end-to-end RTT")
    axis.set_xlabel("Command sequence")
    axis.set_ylabel("Laptop send to result notification (us)")
    finish(figure, axis, output / "adelie_command_total_rtt.png", show)


def plot_adelie_command_breakdown(
    commands: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    if commands.empty:
        return

    stages = [
        ("BLE down (est.)", "ble_down_est_us"),
        ("Korora queue", "korora_queue_to_i2c_us"),
        ("I2C down", "i2c_down_us"),
        ("Fairy action", "fairy_action_us"),
        ("I2C up + poll", "i2c_up_and_poll_us"),
        ("Korora post-ACK", "korora_post_ack_us"),
        ("BLE up (est.)", "ble_up_est_us"),
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
    axis.bar(positions, medians)
    axis.set_xticks(positions)
    axis.set_xticklabels(labels, rotation=30, ha="right")
    axis.set_title("Adelie command latency breakdown (median)")
    axis.set_xlabel("Stage")
    axis.set_ylabel("Latency (us)")
    finish(figure, axis, output / "adelie_command_breakdown.png", show)


def plot_adelie_ble_directions(
    commands: pd.DataFrame,
    output: Path,
    show: bool,
) -> None:
    data = commands[["ble_down_est_us", "ble_up_est_us"]].dropna()
    if len(data) < 2:
        return

    figure, axis = plt.subplots(figsize=(8.5, 5))
    axis.scatter(data["ble_down_est_us"], data["ble_up_est_us"], s=22)
    lower = float(min(data.min()))
    upper = float(max(data.max()))
    if upper > lower:
        guide = np.linspace(lower, upper, 100)
        axis.plot(guide, guide, linestyle="--", linewidth=1, label="equal delay")
    axis.set_title("Adelie BLE one-way delay estimates")
    axis.set_xlabel("Adelie to Korora estimate (us)")
    axis.set_ylabel("Korora to Adelie estimate (us)")
    finish(figure, axis, output / "adelie_ble_direction_comparison.png", show)


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
    before = set(output_dir.glob("*.png"))

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
        selected = {item.strip() for item in node_selection.split(",") if item.strip()}
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
            plot_tracking_state(node, sync, diagnostics, pairs, output_dir, show)
            plot_prediction_error(node, sync, diagnostics, pairs, output_dir, show)
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
            node_sync = sync[sync["node"] == node] if not sync.empty else pd.DataFrame()
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
                plot_tracking_state(node, sync, diagnostics, pairs, output_dir, show)

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
        plot_event_error_distribution(events, output_dir, show, non_reference_selected)

    if REFERENCE_NODE in selected and profile == "full":
        plot_reference_event_spacing(events, output_dir, show)

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

    after = set(output_dir.glob("*.png"))
    return sorted(after - before)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Save curated Korora and Adelie synchronization plots."
    )
    parser.add_argument("--parsed", type=Path, default=Path("parsed"))
    parser.add_argument("--analysis", type=Path, default=Path("analysis"))
    parser.add_argument("--output", type=Path, default=Path("plots"))
    parser.add_argument(
        "--node",
        default="all",
        help="all or a comma-separated selection such as fairy,galapagos,adelie",
    )
    parser.add_argument(
        "--profile",
        choices=["curated", "expanded", "full"],
        default="expanded",
        help=(
            "curated is compact; expanded restores the original remote-node "
            "diagnostics without Korora-only plots; full also adds reference "
            "diagnostics"
        ),
    )
    parser.add_argument(
        "--show",
        action="store_true",
        help="display plots in addition to saving PNG files",
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

    print(f"Plots written to: {args.output}")
    print(f"New plot files: {len(created)}")


if __name__ == "__main__":
    main()
