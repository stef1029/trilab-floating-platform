"""One diagnostics card per Hub / TTL generator / node node."""

from __future__ import annotations

from collections import deque
import math
import time

import pyqtgraph as pg
from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSlider,
    QSizePolicy,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from ...constants import TTL_GENERATOR_ADDRESS, HUB_ADDRESS
from ...models import NodeStats
from ...health import NodeState, node_state
from ..common import StatLabel, repolish, set_pill, signed_us


class NodeCard(QFrame):
    rgb_requested = Signal(int, int, int, int)
    ir_requested = Signal(int, bool)
    audio_requested = Signal(int, int, int, int, int, int, int)
    valve_requested = Signal(int, int)

    def __init__(self, address: int, name: str) -> None:
        super().__init__()
        self.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Maximum)
        self.address = address
        self.name_label = QLabel(name)
        self.name_label.setObjectName("nodeTitle")
        self.state = QLabel("offline")
        self.state.setObjectName("statePill")

        header = QHBoxLayout()
        header.setContentsMargins(0, 0, 0, 0)
        header.setSpacing(8)
        header.addWidget(self.name_label)
        header.addStretch()
        header.addWidget(self.state)

        self.rms = StatLabel()
        self.skew = StatLabel()
        self.rate = StatLabel()
        self.queue = StatLabel()
        self.loss = StatLabel()
        self.rssi = StatLabel()
        self.ttl = StatLabel("waiting for TTL")
        self.light_gate = StatLabel("waiting for event")
        self.light_gate.setObjectName("lightGatePill")
        self.light_gate.setProperty("active", False)
        form = QFormLayout()
        form.setContentsMargins(0, 0, 0, 0)
        form.setVerticalSpacing(4)
        form.setRowWrapPolicy(QFormLayout.DontWrapRows)
        form.addRow("Clock RMS" if address ==
                    HUB_ADDRESS else "Sync RMS", self.rms)
        form.addRow("Clock skew", self.skew)
        form.addRow("Records", self.rate)
        form.addRow("Queue", self.queue)
        loss_name = "BLE / drops" if address == TTL_GENERATOR_ADDRESS else "RS485 / drops"
        form.addRow(loss_name, self.loss)
        form.addRow("RSSI", self.rssi)
        if address in (HUB_ADDRESS, TTL_GENERATOR_ADDRESS):
            form.addRow(
                "TTL input" if address == HUB_ADDRESS else "TTL output",
                self.ttl,
            )
        if address not in (HUB_ADDRESS, TTL_GENERATOR_ADDRESS):
            form.addRow("Light gate", self.light_gate)

        layout = QVBoxLayout(self)
        layout.setSpacing(6)
        layout.setAlignment(Qt.AlignTop)
        layout.addLayout(header)
        layout.addLayout(form)
        self.rms_history: deque[float] = deque(maxlen=120)
        self.rms_plot = pg.PlotWidget()
        self.rms_plot.setFixedHeight(
            240
            if address in (HUB_ADDRESS, TTL_GENERATOR_ADDRESS)
            else 120
        )
        self.rms_plot.setBackground("#211c2d")
        self.rms_plot.showGrid(x=True, y=True, alpha=0.20)
        self.rms_plot.setMouseEnabled(x=False, y=False)
        self._last_clock_exchange_count = 0
        self.clock_rtt_history: deque[float] = deque(maxlen=120)
        self.clock_median_history: deque[float] = deque(maxlen=120)
        self.clock_exchange_history: deque[int] = deque(maxlen=120)
        if address == HUB_ADDRESS:
            self.rms_plot.setLabel("left", "RTT", units="ms")
            self.rms_plot.setLabel("bottom", "Exchange")
            self.rms_plot.addLegend(offset=(5, 5))
            self.clock_rtt_curve = self.rms_plot.plot(
                pen=pg.mkPen("#f0a6ca", width=2), name="Latest RTT"
            )
            self.clock_median_curve = self.rms_plot.plot(
                pen=pg.mkPen("#b99cf4", width=2), name="Rolling median"
            )
            self.rms_curve = None
        else:
            self.rms_plot.setLabel("left", "RMS", units="µs")
            self.rms_plot.setLabel("bottom", "Recent sample")
            self.rms_curve = self.rms_plot.plot(
                pen=pg.mkPen("#b99cf4", width=2)
            )
        layout.addWidget(self.rms_plot)

        if address not in (HUB_ADDRESS, TTL_GENERATOR_ADDRESS):
            layout.addWidget(self._node_controls())

    def _node_controls(self) -> QWidget:
        box = QGroupBox("Controls")
        layout = QVBoxLayout(box)

        rgb = QGridLayout()
        self.red = QSlider(Qt.Horizontal)
        self.green = QSlider(Qt.Horizontal)
        self.blue = QSlider(Qt.Horizontal)
        for slider in (self.red, self.green, self.blue):
            slider.setRange(0, 255)
        rgb.addWidget(QLabel("R"), 0, 0)
        rgb.addWidget(self.red, 0, 1)
        rgb.addWidget(QLabel("G"), 1, 0)
        rgb.addWidget(self.green, 1, 1)
        rgb.addWidget(QLabel("B"), 2, 0)
        rgb.addWidget(self.blue, 2, 1)
        set_rgb = QPushButton("Set RGB")
        set_rgb.clicked.connect(
            lambda: self.rgb_requested.emit(
                self.address,
                self.red.value(),
                self.green.value(),
                self.blue.value(),
            )
        )
        off = QPushButton("RGB off")
        off.clicked.connect(
            lambda: self.rgb_requested.emit(self.address, 0, 0, 0))
        rgb_buttons = QHBoxLayout()
        rgb_buttons.addWidget(set_rgb)
        rgb_buttons.addWidget(off)
        layout.addLayout(rgb)
        layout.addLayout(rgb_buttons)

        # Boards switch their IR beam on at boot (if the light gate passed its
        # self test), so the button starts on. It shows the last request,
        # not a reading from the board.
        self.ir_button = QPushButton("IR emitter on")
        self.ir_button.setCheckable(True)
        self.ir_button.setChecked(True)
        self.ir_button.toggled.connect(self._set_ir_enabled)
        layout.addWidget(self.ir_button)

        self.audio_mode = QComboBox()
        self.audio_mode.addItems(["Off", "Tone", "White noise band"])
        self.frequency = QSpinBox()
        self.frequency.setRange(20, 40_000)
        self.frequency.setValue(10_000)
        self.low_frequency = QSpinBox()
        self.low_frequency.setRange(20, 40_000)
        self.low_frequency.setValue(8_000)
        self.high_frequency = QSpinBox()
        self.high_frequency.setRange(20, 40_000)
        self.high_frequency.setValue(16_000)
        self.amplitude = QSpinBox()
        self.amplitude.setRange(0, 2047)
        self.amplitude.setValue(512)
        self.audio_duration = QSpinBox()
        self.audio_duration.setRange(0, 60_000)
        self.audio_duration.setValue(500)

        audio_form = QFormLayout()
        audio_form.addRow("Audio", self.audio_mode)
        audio_form.addRow("Tone Hz", self.frequency)
        audio_form.addRow("Band low Hz", self.low_frequency)
        audio_form.addRow("Band high Hz", self.high_frequency)
        audio_form.addRow("Amplitude", self.amplitude)
        audio_form.addRow("Duration ms", self.audio_duration)
        audio_button = QPushButton("Apply audio")
        audio_button.clicked.connect(
            lambda: self.audio_requested.emit(
                self.address,
                self.audio_mode.currentIndex(),
                self.frequency.value(),
                self.low_frequency.value(),
                self.high_frequency.value(),
                self.amplitude.value(),
                self.audio_duration.value(),
            )
        )
        layout.addLayout(audio_form)
        layout.addWidget(audio_button)

        self.valve_duration = QSpinBox()
        self.valve_duration.setRange(1, 250)
        self.valve_duration.setValue(20)
        valve_button = QPushButton("Open valve")
        valve_button.setObjectName("dangerButton")
        valve_button.clicked.connect(
            lambda: self.valve_requested.emit(
                self.address, self.valve_duration.value()
            )
        )
        valve_row = QHBoxLayout()
        valve_row.addWidget(self.valve_duration)
        valve_row.addWidget(QLabel("ms"))
        valve_row.addWidget(valve_button)
        layout.addLayout(valve_row)
        return box

    def _set_ir_enabled(self, enabled: bool) -> None:
        self.ir_button.setText(
            "IR emitter on" if enabled else "IR emitter off"
        )
        self.ir_requested.emit(self.address, enabled)

    def update_stats(self, stats: NodeStats) -> None:
        self.name_label.setText(stats.name)
        state = node_state(stats)
        set_pill(
            self.state,
            state is not NodeState.OFFLINE,
            {
                NodeState.READY: "tracking",
                NodeState.ONLINE: "online",
                NodeState.OFFLINE: "offline",
            }[state],
        )
        if self.address != HUB_ADDRESS:
            self.rms.setText(
                "-" if stats.rms_ns != stats.rms_ns else f"{stats.rms_ns / 1000:.2f} µs"
            )
        if (
            self.address != HUB_ADDRESS
            and stats.rms_ns == stats.rms_ns
            and self.rms_curve is not None
        ):
            self.rms_history.append(stats.rms_ns / 1000)
            self.rms_curve.setData(list(self.rms_history))
        if self.address != HUB_ADDRESS:
            self.skew.setText(f"{stats.skew_ppb / 1000:.2f} ppm")
        self.rate.setText(f"{stats.record_rate_hz:.1f} Hz")
        self.queue.setText(str(stats.queue_depth))
        self.loss.setText(
            f"{stats.dropped_records} dropped · {stats.transport_errors} errors"
        )
        self.rssi.setText(
            "-" if stats.rssi_dbm is None else f"{stats.rssi_dbm} dBm")
        if self.address in (HUB_ADDRESS, TTL_GENERATOR_ADDRESS):
            if stats.last_ttl_ns == 0:
                if self.address == HUB_ADDRESS:
                    self.ttl.setText(
                        f"{stats.ttl_capture_count} hardware edges · "
                        f"{stats.ttl_capture_drops} queue drops"
                    )
                else:
                    self.ttl.setText("waiting for TTL")
            else:
                age_s = max(
                    0.0, (time.perf_counter_ns() - stats.last_ttl_ns) / 1e9)
                error = signed_us(stats.last_ttl_error_us)
                suffix = (
                    f" · {stats.ttl_capture_count} hardware edges"
                    if self.address == HUB_ADDRESS else ""
                )
                self.ttl.setText(
                    f"seq {stats.last_ttl_sequence} · {error} · "
                    f"{age_s:.1f} s ago{suffix}"
                )
        if self.address not in (HUB_ADDRESS, TTL_GENERATOR_ADDRESS):
            age_ns = time.perf_counter_ns() - stats.last_light_gate_ns
            active = (
                stats.last_light_gate_ns != 0
                and 0 <= age_ns < 1_000_000_000
            )
            if active:
                text = f"BEAM BREAK · {stats.light_gate_events}"
            elif stats.last_light_gate_ns == 0:
                text = "waiting for event"
            else:
                age_s = max(0.0, age_ns / 1e9)
                text = f"{stats.light_gate_events} events · {age_s:.1f} s ago"
            self.light_gate.setText(text)
            self.light_gate.setProperty("active", active)
            repolish(self.light_gate)

    def update_clock_diagnostics(self, values: dict[str, object]) -> None:
        if self.address != HUB_ADDRESS:
            return

        rms = float(values.get("clock_rms_us", math.nan))
        skew = float(values.get("clock_skew_ppm", math.nan))
        self.rms.setText(
            "-" if not math.isfinite(rms) else f"{rms:.3f} µs"
        )
        self.skew.setText(
            "-" if not math.isfinite(skew) else f"{skew:+.2f} ppm"
        )

        exchange_count = int(values.get("clock_exchange_count", 0))
        if exchange_count < self._last_clock_exchange_count:
            self.clock_exchange_history.clear()
            self.clock_rtt_history.clear()
            self.clock_median_history.clear()
        if exchange_count == self._last_clock_exchange_count:
            return
        self._last_clock_exchange_count = exchange_count
        latest_us = float(values.get("clock_last_rtt_us", math.nan))
        median_us = float(values.get("clock_median_rtt_us", math.nan))
        if not math.isfinite(latest_us):
            return
        self.clock_exchange_history.append(exchange_count)
        self.clock_rtt_history.append(latest_us / 1000.0)
        self.clock_median_history.append(
            median_us / 1000.0 if math.isfinite(median_us) else math.nan
        )
        x = list(self.clock_exchange_history)
        self.clock_rtt_curve.setData(x, list(self.clock_rtt_history))
        self.clock_median_curve.setData(x, list(self.clock_median_history))
        finite = [
            value
            for value in self.clock_rtt_history
            if math.isfinite(value)
        ]
        self.rms_plot.setYRange(
            0.0, max(1.0, max(finite, default=0.0) * 1.15), padding=0
        )
