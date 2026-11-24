"""Hub local sensors (IMU, magnetometer) and power (nPM1300)."""

from __future__ import annotations


from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
)

from ...models import LocalSensorState
from .pose import LivePoseWidget
from ..common import StatLabel, set_pill


class LocalSensorsPanel(QFrame):
    status_led_requested = Signal(int, bool)

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)

        title = QLabel("Hub local sensors")
        title.setObjectName("nodeTitle")
        subtitle = QLabel("LSM6DSV32X + MMC5983MA · shared I²C")
        subtitle.setStyleSheet("color: #9b91ad;")

        title_row = QHBoxLayout()
        title_row.addWidget(title)
        title_row.addWidget(subtitle)
        title_row.addStretch()

        self.i2c_status = QLabel("I²C waiting")
        self.imu_status = QLabel("IMU waiting")
        self.mag_status = QLabel("MAG waiting")
        for label in (self.i2c_status, self.imu_status, self.mag_status):
            label.setObjectName("statePill")
            label.setProperty("online", False)
            title_row.addWidget(label)

        self.accel = StatLabel("-")
        self.gyro = StatLabel("-")
        self.mag = StatLabel("-")
        self.rate = StatLabel("-")
        self.raw_dump = StatLabel("-")
        self.i2c_errors = StatLabel("0")
        self.power = StatLabel("nPM1300 waiting")
        self.charge = StatLabel("Charge status waiting")
        self.battery = StatLabel("Battery telemetry waiting")

        self.led1 = QPushButton("LED 1")
        self.led2 = QPushButton("LED 2")
        for button in (self.led1, self.led2):
            button.setCheckable(True)
            button.setEnabled(False)
        self.led1.toggled.connect(
            lambda enabled: self.status_led_requested.emit(1, enabled)
        )
        self.led2.toggled.connect(
            lambda enabled: self.status_led_requested.emit(2, enabled)
        )

        values = QGridLayout()
        values.addWidget(QLabel("Acceleration"), 0, 0)
        values.addWidget(self.accel, 0, 1)
        values.addWidget(QLabel("Gyroscope"), 1, 0)
        values.addWidget(self.gyro, 1, 1)
        values.addWidget(QLabel("Magnetic field"), 2, 0)
        values.addWidget(self.mag, 2, 1)
        values.addWidget(QLabel("Acquisition"), 3, 0)
        values.addWidget(self.rate, 3, 1)
        values.addWidget(QLabel("Raw IMU dump"), 4, 0)
        values.addWidget(self.raw_dump, 4, 1)
        values.addWidget(QLabel("I²C errors"), 5, 0)
        values.addWidget(self.i2c_errors, 5, 1)
        values.setColumnStretch(1, 1)

        power_box = QVBoxLayout()
        power_title = QLabel("Power")
        power_title.setStyleSheet("font-weight: 700; color: #cebfff;")
        power_box.addWidget(power_title)
        power_box.addWidget(self.power)
        power_box.addWidget(self.charge)
        power_box.addWidget(self.battery)
        led_row = QHBoxLayout()
        led_row.addWidget(QLabel("Programmable"))
        led_row.addWidget(self.led1)
        led_row.addWidget(self.led2)
        power_box.addLayout(led_row)
        power_box.addStretch()

        pose_box = QVBoxLayout()
        pose_header = QHBoxLayout()
        pose_title = QLabel("Live pose")
        pose_title.setStyleSheet("font-weight: 700; color: #cebfff;")
        reset_pose = QPushButton("Reset view")
        reset_pose.clicked.connect(self._reset_pose)
        pose_header.addWidget(pose_title)
        pose_header.addStretch()
        pose_header.addWidget(reset_pose)
        pose_box.addLayout(pose_header)
        self.pose = LivePoseWidget()
        pose_box.addWidget(self.pose, 1)

        self._last_sample_count = -1

        body = QHBoxLayout()
        body.addLayout(values, 3)
        body.addLayout(power_box, 2)
        body.addLayout(pose_box, 5)

        layout = QVBoxLayout(self)
        layout.addLayout(title_row)
        layout.addLayout(body)

    @staticmethod
    def _vector(values: tuple[int, int, int], scale: float, unit: str) -> str:
        return (
            f"X {values[0] / scale:+.3f} · "
            f"Y {values[1] / scale:+.3f} · "
            f"Z {values[2] / scale:+.3f} {unit}"
        )

    def _reset_pose(self) -> None:
        self.pose.reset_pose()

    def update_state(self, state: LocalSensorState) -> None:
        set_pill(
            self.i2c_status, state.i2c_ready,
            "I²C ready" if state.i2c_ready else "I²C offline",
        )
        set_pill(
            self.imu_status, state.imu_present,
            "IMU online" if state.imu_present else "IMU missing",
        )
        set_pill(
            self.mag_status, state.magnetometer_present,
            "MAG online" if state.magnetometer_present else "MAG missing",
        )

        self.accel.setText(self._vector(state.accel_mg, 1000.0, "g"))
        self.gyro.setText(self._vector(state.gyro_mdps, 1000.0, "°/s"))
        self.mag.setText(self._vector(state.mag_milligauss, 1000.0, "G"))
        self.rate.setText(
            f"{state.sample_rate_hz} Hz · {state.sample_count:,} samples"
            if state.sample_rate_hz
            else "waiting for sensor telemetry"
        )
        self.raw_dump.setText(
            f"{state.raw_samples_received:,} samples · "
            f"{state.raw_chunks_received:,} chunks · "
            f"seq {state.last_chunk_sequence}"
        )
        self.i2c_errors.setText(f"{state.i2c_errors:,}")

        self.power.setText(state.power_source or "power source unknown")
        self.led1.setEnabled(state.pmic_present)
        self.led2.setEnabled(state.pmic_present)
        if state.pmic_present:
            charge_text = state.charge_status or "Charger status unavailable"
            if state.charger_error:
                charge_text += f" · ERROR: {state.charger_error}"
            self.charge.setText(charge_text)
            details: list[str] = []
            if state.battery_millivolts is not None:
                details.append(f"{state.battery_millivolts / 1000:.3f} V")
            if state.battery_current_ma is not None:
                details.append(f"{state.battery_current_ma:+d} mA")
            if state.battery_soc_per_mille is not None:
                details.append(f"{state.battery_soc_per_mille / 10:.1f}% SoC")
            if state.battery_health_per_mille is not None:
                details.append(
                    f"{state.battery_health_per_mille / 10:.1f}% health")
            self.battery.setText(" · ".join(
                details) if details else "PMIC online")
        else:
            self.charge.setText("nPM1300 unavailable")
            self.battery.setText("nPM1300 unavailable")

        if state.sample_count == self._last_sample_count or not state.imu_present:
            return
        self._last_sample_count = state.sample_count
        self.pose.update_imu(
            state.accel_mg,
            state.gyro_mdps,
            state.mag_milligauss,
            state.last_update_ns,
            state.imu_present,
        )

