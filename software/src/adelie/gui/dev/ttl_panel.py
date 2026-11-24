"""TTL loop timing: TTL generator output vs Hub capture."""

from __future__ import annotations

from collections import deque
import math

import pyqtgraph as pg
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QLabel,
    QSizePolicy,
    QVBoxLayout,
)

from ...models import TtlStats
from ..common import StatLabel, signed_us


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
        self.live_path = StatLabel("waiting for TTL generator output")

        form = QGridLayout()
        form.addWidget(QLabel("Sequence"), 0, 0)
        form.addWidget(self.sequence, 0, 1)
        form.addWidget(QLabel("Latest record"), 0, 2)
        form.addWidget(self.latest_record, 0, 3)
        form.addWidget(QLabel("Target"), 1, 0)
        form.addWidget(self.target, 1, 1)
        form.addWidget(QLabel("TTL generator output"), 1, 2)
        form.addWidget(self.generated, 1, 3)
        form.addWidget(QLabel("Hub input"), 2, 0)
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
            name="TTL generator output",
        )
        self.input_curve = self.plot.plot(
            pen=pg.mkPen("#f0a6ca", width=2),
            name="Hub input",
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
        self.target.setText(self._tick_time(ttl.target_hub_ticks))
        self.generated.setText(self._tick_time(ttl.generated_hub_ticks))
        self.captured.setText(self._tick_time(ttl.captured_hub_ticks))
        self.output_error.setText(signed_us(ttl.output_error_us))

        input_error = (
            ttl.result_error_us
            if math.isfinite(ttl.result_error_us)
            else ttl.input_error_us
        )

        self.input_error.setText(signed_us(input_error))
        self.loop_delay.setText(signed_us(ttl.loop_delay_us))

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
        generated = int(values["ttl_generator_ttl_generated"])
        captured = int(values["hub_ttl_captured_records"])
        edges = int(values["hub_ttl_capture_edges"])
        drops = int(values["hub_ttl_capture_drops"])
        self.live_path.setText(
            f"Generated {generated} · "
            f"Edges {edges} · "
            f"Records {captured} · Drops {drops}"
        )

