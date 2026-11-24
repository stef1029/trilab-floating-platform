"""Adelie <-> Hub BLE link, RS485 gateway and host clock diagnostics."""

from __future__ import annotations

from collections import deque

import pyqtgraph as pg
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QLabel,
    QSizePolicy,
    QVBoxLayout,
)

from ..common import StatLabel, us


class TransportDiagnostics(QFrame):
    def __init__(self) -> None:
        super().__init__()
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        title = QLabel("Adelie <-> Hub diagnostics")
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
        form.addWidget(QLabel("Hub gateway"), 1, 2)
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
            pen=pg.mkPen("#f6c177", width=2), name="Hub BLE drops"
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
            f"RMS {us(rms)} · median RTT {us(rtt)} · "
            f"{int(values['clock_points'])} points"
            if bool(values["clock_valid"]) else
            f"acquiring · {int(values['clock_points'])} points"
        )
        rs485_total = int(values["hub_rs485_errors"])
        retry_total = int(values["hub_rs485_retries"])
        timeout_total = int(values["hub_rs485_timeouts"])
        decode_total = int(values["hub_rs485_decode_errors"])
        reassembly_total = int(values["hub_rs485_reassembly_errors"])
        transmit_total = int(values["hub_rs485_transmit_errors"])
        drop_total = int(values["hub_ble_dropped"])
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

