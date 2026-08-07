from __future__ import annotations

from collections import deque
import math
import time

import pyqtgraph as pg
from PySide6.QtCore import Signal
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
from PySide6.QtCore import Qt

from .constants import GALAPAGOS_ADDRESS, KORORA_ADDRESS
from .models import NodeStats, TtlStats


class StatLabel(QLabel):
    def __init__(self, text: str = "-") -> None:
        super().__init__(text)
        self.setTextInteractionFlags(Qt.TextSelectableByMouse)
        # Live counters must not change the minimum width of their parent
        # layout whenever another digit is added.
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)


def _signed_us(value: float) -> str:
    return "-" if not math.isfinite(value) else f"{value:+.3f} µs"


def _us(value: float) -> str:
    return "-" if not math.isfinite(value) else f"{value:.3f} µs"


class TtlDiagnostics(QFrame):
    def __init__(self) -> None:
        super().__init__()
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        title = QLabel("TTL loop timing")
        title.setObjectName("nodeTitle")
        self.sequence = StatLabel("waiting for TTL records")
        self.latest_record = StatLabel("-")
        self.target = StatLabel("-")
        self.generated = StatLabel("-")
        self.captured = StatLabel("-")
        self.output_error = StatLabel("-")
        self.input_error = StatLabel("-")
        self.loop_delay = StatLabel("-")
        self.live_path = StatLabel("waiting for Galapagos output")

        form = QGridLayout()
        form.addWidget(QLabel("Sequence"), 0, 0)
        form.addWidget(self.sequence, 0, 1)
        form.addWidget(QLabel("Latest record"), 0, 2)
        form.addWidget(self.latest_record, 0, 3)
        form.addWidget(QLabel("Target"), 1, 0)
        form.addWidget(self.target, 1, 1)
        form.addWidget(QLabel("Galapagos output"), 1, 2)
        form.addWidget(self.generated, 1, 3)
        form.addWidget(QLabel("Korora input"), 2, 0)
        form.addWidget(self.captured, 2, 1)
        form.addWidget(QLabel("Output error"), 2, 2)
        form.addWidget(self.output_error, 2, 3)
        form.addWidget(QLabel("Input error"), 3, 0)
        form.addWidget(self.input_error, 3, 1)
        form.addWidget(QLabel("Output to input"), 3, 2)
        form.addWidget(self.loop_delay, 3, 3)
        form.addWidget(QLabel("Live path"), 4, 0)
        form.addWidget(self.live_path, 4, 1, 1, 3)
        form.setColumnStretch(1, 1)
        form.setColumnStretch(3, 1)

        self.plot = pg.PlotWidget()
        self.plot.setFixedHeight(180)
        self.plot.setBackground("#211c2d")
        self.plot.setLabel("left", "Timing error", units="µs")
        self.plot.setLabel("bottom", "TTL sequence")
        self.plot.getAxis("left").setWidth(68)
        self.plot.showGrid(x=True, y=True, alpha=0.22)
        self.plot.setMouseEnabled(x=False, y=False)
        self.plot.addLegend(offset=(8, 8))
        self.plot.addItem(
            pg.InfiniteLine(pos=0, angle=0, pen=pg.mkPen("#6d647c", width=1))
        )
        self.output_curve = self.plot.plot(
            pen=pg.mkPen("#b99cf4", width=2),
            name="Galapagos output",
        )
        self.input_curve = self.plot.plot(
            pen=pg.mkPen("#f0a6ca", width=2),
            name="Korora input",
        )
        self._session_id: int | None = None
        self._sequences: deque[int] = deque(maxlen=160)
        self._output_errors: deque[float] = deque(maxlen=160)
        self._input_errors: deque[float] = deque(maxlen=160)

        layout = QVBoxLayout(self)
        layout.addWidget(title)
        layout.addLayout(form)
        layout.addWidget(self.plot)

    @staticmethod
    def _tick_time(ticks: int | None) -> str:
        return "-" if ticks is None else f"{ticks / 16_000_000:.6f} s"

    def update_ttl(self, ttl: TtlStats) -> None:
        max_points = 30

        if self._session_id != ttl.session_id:
            self._session_id = ttl.session_id
            self._sequences.clear()
            self._output_errors.clear()
            self._input_errors.clear()

        self.sequence.setText(f"{ttl.sequence} · session {ttl.session_id}")
        self.latest_record.setText(ttl.latest_record.replace("_", " "))
        self.target.setText(self._tick_time(ttl.target_korora_ticks))
        self.generated.setText(self._tick_time(ttl.generated_korora_ticks))
        self.captured.setText(self._tick_time(ttl.captured_korora_ticks))
        self.output_error.setText(_signed_us(ttl.output_error_us))

        input_error = (
            ttl.result_error_us
            if math.isfinite(ttl.result_error_us)
            else ttl.input_error_us
        )

        self.input_error.setText(_signed_us(input_error))
        self.loop_delay.setText(_signed_us(ttl.loop_delay_us))

        # Update the current sequence or append a new one.
        if self._sequences and self._sequences[-1] == ttl.sequence:
            self._output_errors[-1] = ttl.output_error_us
            self._input_errors[-1] = input_error
        else:
            self._sequences.append(ttl.sequence)
            self._output_errors.append(ttl.output_error_us)
            self._input_errors.append(input_error)

        # Display only the latest 30 points.
        x = list(self._sequences)[-max_points:]
        output = list(self._output_errors)[-max_points:]
        captured = list(self._input_errors)[-max_points:]

        # symbol=None produces lines without point markers.
        self.output_curve.setData(x, output, symbol=None)
        self.input_curve.setData(x, captured, symbol=None)

        # Keep the X-axis fitted to the displayed points.
        if x:
            if len(x) == 1:
                self.plot.setXRange(x[0] - 1, x[0] + 1, padding=0)
            else:
                self.plot.setXRange(x[0], x[-1], padding=0.02)

        # Scale the Y-axis using only the displayed finite values.
        finite = [
            abs(value)
            for value in output + captured
            if math.isfinite(value)
        ]
        scale = max(5.0, max(finite, default=0.0) * 1.15)
        self.plot.setYRange(-scale, scale, padding=0)

    def update_diagnostics(self, values: dict[str, object]) -> None:
        generated = int(values["galapagos_ttl_generated"])
        captured = int(values["korora_ttl_captured_records"])
        edges = int(values["korora_ttl_capture_edges"])
        drops = int(values["korora_ttl_capture_drops"])
        self.live_path.setText(
            f"Generated {generated} · "
            f"Edges {edges} · "
            f"Records {captured} · Drops {drops}"
        )


class TransportDiagnostics(QFrame):
    def __init__(self) -> None:
        super().__init__()
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        title = QLabel("Adelie <-> Korora diagnostics")
        title.setObjectName("nodeTitle")
        self.link = StatLabel("disconnected")
        self.receive = StatLabel("-")
        self.host_errors = StatLabel("-")
        self.clock = StatLabel("-")
        self.gateway = StatLabel("-")
        self.last_error = StatLabel("none")

        form = QGridLayout()
        form.addWidget(QLabel("BLE link"), 0, 0)
        form.addWidget(self.link, 0, 1)
        form.addWidget(QLabel("Receive"), 1, 0)
        form.addWidget(self.receive, 1, 1)
        form.addWidget(QLabel("Host faults"), 2, 0)
        form.addWidget(self.host_errors, 2, 1)
        form.addWidget(QLabel("Clock exchange"), 0, 2)
        form.addWidget(self.clock, 0, 3)
        form.addWidget(QLabel("Korora gateway"), 1, 2)
        form.addWidget(self.gateway, 1, 3)
        form.addWidget(QLabel("Last BLE fault"), 2, 2)
        form.addWidget(self.last_error, 2, 3)
        form.setColumnStretch(1, 1)
        form.setColumnStretch(3, 1)

        self.plot = pg.PlotWidget()
        self.plot.setFixedHeight(180)
        self.plot.setBackground("#211c2d")
        self.plot.setLabel("left", "New errors", units="errors/s")
        self.plot.setLabel("bottom", "Connected time", units="s")
        self.plot.getAxis("left").setWidth(68)
        self.plot.showGrid(x=True, y=True, alpha=0.22)
        self.plot.setMouseEnabled(x=False, y=False)
        self.plot.addLegend(offset=(8, 8))
        self.host_curve = self.plot.plot(
            pen=pg.mkPen("#f0a6ca", width=2), name="Adelie BLE parser"
        )
        self.decode_curve = self.plot.plot(
            pen=pg.mkPen("#b99cf4", width=2), name="RS485 decode"
        )
        self.timeout_curve = self.plot.plot(
            pen=pg.mkPen("#9ccfd8", width=2), name="RS485 timeout"
        )
        self.drop_curve = self.plot.plot(
            pen=pg.mkPen("#f6c177", width=2), name="Korora BLE drops"
        )
        self._origin_ns: int | None = None
        self._counter_samples: deque[
            tuple[int, int, int, int, int, int, int, int]
        ] = deque()
        self._times: deque[float] = deque(maxlen=240)
        self._host_rates: deque[float] = deque(maxlen=240)
        self._decode_rates: deque[float] = deque(maxlen=240)
        self._timeout_rates: deque[float] = deque(maxlen=240)
        self._drop_rates: deque[float] = deque(maxlen=240)

        layout = QVBoxLayout(self)
        layout.addWidget(title)
        layout.addLayout(form)
        layout.addWidget(self.plot)

    def update_diagnostics(self, values: dict[str, object]) -> None:
        sample_ns = int(values["sample_ns"])
        if self._origin_ns is None:
            self._origin_ns = sample_ns
        connected = bool(values["ble_connected"])
        self.link.setText(
            f"{'connected' if connected else 'disconnected'} · "
            f"ATT {int(values['att_write_size'])} B · "
            f"payload {int(values['fragment_payload'])} B · "
            f"TX queue {int(values['outgoing_queue'])}"
        )
        age = values.get("last_rx_age_ms")
        if age is None:
            self.receive.setText("waiting")
        else:
            self.receive.setText(
                f"{int(values['rx_messages'])} messages · "
                f"{int(values['rx_frames'])} frames · "
                f"last {float(age):.1f} ms ago"
            )
        host_total = (
            int(values["rx_decode_errors"])
            + int(values["rx_reassembly_errors"])
            + int(values["rx_address_errors"])
            + int(values["tx_write_errors"])
        )
        self.host_errors.setText(
            f"decode {int(values['rx_decode_errors'])} · "
            f"reassembly {int(values['rx_reassembly_errors'])} · "
            f"address {int(values['rx_address_errors'])} · "
            f"write {int(values['tx_write_errors'])}"
        )
        rms = float(values["clock_rms_us"])
        rtt = float(values["clock_median_rtt_us"])
        self.clock.setText(
            f"RMS {_us(rms)} · median RTT {_us(rtt)} · "
            f"{int(values['clock_points'])} points"
            if bool(values["clock_valid"]) else
            f"acquiring · {int(values['clock_points'])} points"
        )
        rs485_total = int(values["korora_rs485_errors"])
        retry_total = int(values["korora_rs485_retries"])
        timeout_total = int(values["korora_rs485_timeouts"])
        decode_total = int(values["korora_rs485_decode_errors"])
        reassembly_total = int(values["korora_rs485_reassembly_errors"])
        transmit_total = int(values["korora_rs485_transmit_errors"])
        drop_total = int(values["korora_ble_dropped"])
        if self._counter_samples and (
            host_total < self._counter_samples[-1][1]
            or rs485_total < self._counter_samples[-1][2]
            or drop_total < self._counter_samples[-1][3]
            or decode_total < self._counter_samples[-1][4]
            or timeout_total < self._counter_samples[-1][5]
            or reassembly_total < self._counter_samples[-1][6]
            or transmit_total < self._counter_samples[-1][7]
        ):
            self._counter_samples.clear()
            self._times.clear()
            self._host_rates.clear()
            self._decode_rates.clear()
            self._timeout_rates.clear()
            self._drop_rates.clear()
            self._origin_ns = sample_ns
        self._counter_samples.append(
            (
                sample_ns,
                host_total,
                rs485_total,
                drop_total,
                decode_total,
                timeout_total,
                reassembly_total,
                transmit_total,
            )
        )
        cutoff = sample_ns - 5_000_000_000
        while (
            len(self._counter_samples) > 2
            and self._counter_samples[1][0] <= cutoff
        ):
            self._counter_samples.popleft()
        host_rate = rs485_rate = drop_rate = 0.0
        decode_rate = timeout_rate = 0.0
        if len(self._counter_samples) >= 2:
            previous = self._counter_samples[0]
            previous_ns = previous[0]
            elapsed = (sample_ns - previous_ns) / 1e9
            if elapsed > 0:
                host_rate = max(0, host_total - previous[1]) / elapsed
                rs485_rate = max(0, rs485_total - previous[2]) / elapsed
                drop_rate = max(0, drop_total - previous[3]) / elapsed
                decode_rate = max(0, decode_total - previous[4]) / elapsed
                timeout_rate = max(0, timeout_total - previous[5]) / elapsed
        self.gateway.setText(
            f"RS485 {rs485_total} ({rs485_rate:.2f}/s) · "
            f"decode {decode_total} · timeout {timeout_total} · "
            f"reassembly {reassembly_total} · TX {transmit_total} · "
            f"retries {retry_total} · BLE drops {drop_total}"
        )
        full_error = str(values.get("last_error") or "none")
        self.last_error.setText(
            full_error if len(full_error) <= 72 else full_error[:69] + "..."
        )
        self.last_error.setToolTip(full_error)

        self._times.append((sample_ns - self._origin_ns) / 1e9)
        self._host_rates.append(host_rate)
        self._decode_rates.append(decode_rate)
        self._timeout_rates.append(timeout_rate)
        self._drop_rates.append(drop_rate)
        x = list(self._times)
        self.host_curve.setData(x, list(self._host_rates))
        self.decode_curve.setData(x, list(self._decode_rates))
        self.timeout_curve.setData(x, list(self._timeout_rates))
        self.drop_curve.setData(x, list(self._drop_rates))
        maximum = max(
            1.0,
            max(self._host_rates, default=0.0),
            max(self._decode_rates, default=0.0),
            max(self._timeout_rates, default=0.0),
            max(self._drop_rates, default=0.0),
        )
        self.plot.setYRange(0, maximum * 1.15, padding=0)


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
                    KORORA_ADDRESS else "Sync RMS", self.rms)
        form.addRow("Clock skew", self.skew)
        form.addRow("Records", self.rate)
        form.addRow("Queue", self.queue)
        loss_name = "BLE / drops" if address == GALAPAGOS_ADDRESS else "RS485 / drops"
        form.addRow(loss_name, self.loss)
        form.addRow("RSSI", self.rssi)
        if address in (KORORA_ADDRESS, GALAPAGOS_ADDRESS):
            form.addRow(
                "TTL input" if address == KORORA_ADDRESS else "TTL output",
                self.ttl,
            )
        if address not in (KORORA_ADDRESS, GALAPAGOS_ADDRESS):
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
            if address in (KORORA_ADDRESS, GALAPAGOS_ADDRESS)
            else 120
        )
        self.rms_plot.setBackground("#211c2d")
        self.rms_plot.showGrid(x=True, y=True, alpha=0.20)
        self.rms_plot.setMouseEnabled(x=False, y=False)
        self._last_clock_exchange_count = 0
        self.clock_rtt_history: deque[float] = deque(maxlen=120)
        self.clock_median_history: deque[float] = deque(maxlen=120)
        self.clock_exchange_history: deque[int] = deque(maxlen=120)
        if address == KORORA_ADDRESS:
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

        if address not in (KORORA_ADDRESS, GALAPAGOS_ADDRESS):
            layout.addWidget(self._fairy_controls())

    def _fairy_controls(self) -> QWidget:
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

        self.ir_button = QPushButton("IR emitter off")
        self.ir_button.setCheckable(True)
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
        self.state.setText(
            "tracking" if stats.synchronized else (
                "online" if stats.connected else "offline")
        )
        self.state.setProperty(
            "online", stats.connected
        )
        self.style().unpolish(self.state)
        self.style().polish(self.state)
        if self.address != KORORA_ADDRESS:
            self.rms.setText(
                "-" if stats.rms_ns != stats.rms_ns else f"{stats.rms_ns / 1000:.2f} µs"
            )
        if (
            self.address != KORORA_ADDRESS
            and stats.rms_ns == stats.rms_ns
            and self.rms_curve is not None
        ):
            self.rms_history.append(stats.rms_ns / 1000)
            self.rms_curve.setData(list(self.rms_history))
        if self.address != KORORA_ADDRESS:
            self.skew.setText(f"{stats.skew_ppb / 1000:.2f} ppm")
        self.rate.setText(f"{stats.record_rate_hz:.1f} Hz")
        self.queue.setText(str(stats.queue_depth))
        self.loss.setText(
            f"{stats.dropped_records} dropped · {stats.transport_errors} errors"
        )
        self.rssi.setText(
            "-" if stats.rssi_dbm is None else f"{stats.rssi_dbm} dBm")
        if self.address in (KORORA_ADDRESS, GALAPAGOS_ADDRESS):
            if stats.last_ttl_ns == 0:
                if self.address == KORORA_ADDRESS:
                    self.ttl.setText(
                        f"{stats.ttl_capture_count} hardware edges · "
                        f"{stats.ttl_capture_drops} queue drops"
                    )
                else:
                    self.ttl.setText("waiting for TTL")
            else:
                age_s = max(
                    0.0, (time.perf_counter_ns() - stats.last_ttl_ns) / 1e9)
                error = _signed_us(stats.last_ttl_error_us)
                suffix = (
                    f" · {stats.ttl_capture_count} hardware edges"
                    if self.address == KORORA_ADDRESS else ""
                )
                self.ttl.setText(
                    f"seq {stats.last_ttl_sequence} · {error} · "
                    f"{age_s:.1f} s ago{suffix}"
                )
        if self.address not in (KORORA_ADDRESS, GALAPAGOS_ADDRESS):
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
            self.style().unpolish(self.light_gate)
            self.style().polish(self.light_gate)

    def update_clock_diagnostics(self, values: dict[str, object]) -> None:
        import math as _math

        if self.address != KORORA_ADDRESS:
            return

        # Lazily create these members so this method remains a drop in change
        # for NodeCard versions that predate the Korora clock graph.
        if not hasattr(self, "clock_rtt_history"):
            existing_curve = getattr(self, "rms_curve", None)
            if existing_curve is not None:
                existing_curve.setVisible(False)
            self.rms_plot.showAxis("left")
            self.rms_plot.showAxis("bottom")
            self.rms_plot.setTitle("Adelie <-> Korora clock exchange")
            self.rms_plot.setLabel("left", "RTT", units="ms")
            self.rms_plot.setLabel("bottom", "Exchange")
            self.rms_plot.showGrid(x=True, y=True, alpha=0.20)
            self.rms_plot.addLegend(offset=(5, 5))
            self.clock_rtt_curve = self.rms_plot.plot(
                pen=pg.mkPen("#f0a6ca", width=2), name="Latest RTT"
            )
            self.clock_median_curve = self.rms_plot.plot(
                pen=pg.mkPen("#b99cf4", width=2), name="Rolling median"
            )
            self._last_clock_exchange_count = 0
            self.clock_rtt_history = deque(maxlen=120)
            self.clock_median_history = deque(maxlen=120)
            self.clock_exchange_history = deque(maxlen=120)

        rms = float(values.get("clock_rms_us", _math.nan))
        skew = float(values.get("clock_skew_ppm", _math.nan))
        self.rms.setText(
            "-" if not _math.isfinite(rms) else f"{rms:.3f} µs"
        )
        self.skew.setText(
            "-" if not _math.isfinite(skew) else f"{skew:+.2f} ppm"
        )

        exchange_count = int(values.get("clock_exchange_count", 0))
        if exchange_count < self._last_clock_exchange_count:
            self.clock_exchange_history.clear()
            self.clock_rtt_history.clear()
            self.clock_median_history.clear()
        if exchange_count == self._last_clock_exchange_count:
            return
        self._last_clock_exchange_count = exchange_count
        latest_us = float(values.get("clock_last_rtt_us", _math.nan))
        median_us = float(values.get("clock_median_rtt_us", _math.nan))
        if not _math.isfinite(latest_us):
            return
        self.clock_exchange_history.append(exchange_count)
        self.clock_rtt_history.append(latest_us / 1000.0)
        self.clock_median_history.append(
            median_us / 1000.0 if _math.isfinite(median_us) else _math.nan
        )
        x = list(self.clock_exchange_history)
        self.clock_rtt_curve.setData(x, list(self.clock_rtt_history))
        self.clock_median_curve.setData(x, list(self.clock_median_history))
        finite = [
            value
            for value in self.clock_rtt_history
            if _math.isfinite(value)
        ]
        self.rms_plot.setYRange(
            0.0, max(1.0, max(finite, default=0.0) * 1.15), padding=0
        )
