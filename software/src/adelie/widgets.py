from __future__ import annotations

from collections import deque
import math
import time

import pyqtgraph as pg
from PySide6.QtCore import QPointF, QTimer, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF
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
from .models import LocalSensorState, NodeStats, TtlStats


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


class LivePoseWidget(QWidget):
    """Lightweight 3-D-ish Korora pose toy driven by live IMU telemetry.

    This intentionally lives in the desktop UI rather than the firmware. It is
    a visual aid, not a navigation solution: gyro rates provide the motion and
    accelerometer / magnetometer measurements slowly pull the displayed attitude
    back toward a plausible absolute orientation.
    """

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumSize(360, 220)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self._roll = 0.0
        self._pitch = 0.0
        self._yaw = 0.0
        self._display_roll = 0.0
        self._display_pitch = 0.0
        self._display_yaw = 0.0

        # A deliberately playful translation model.  We do not integrate
        # acceleration into real position (that would drift almost
        # immediately).  Instead, linear acceleration gives the model a
        # short displacement impulse and a damped spring pulls it back to
        # the origin.  The result reads visually as "the board moved" while
        # remaining bounded.
        self._motion = [0.0, 0.0, 0.0]
        self._motion_velocity = [0.0, 0.0, 0.0]
        self._motion_accel = [0.0, 0.0, 0.0]
        self._last_animation_ns = time.monotonic_ns()

        # Fixed camera/view transform.  This is independent of sensor pose so
        # a zeroed Korora starts in a useful three-quarter perspective view.
        # Camera uses only X/Y tilt.  Do not rotate the camera around Z here:
        # a Z rotation makes a zero-pose board look physically rolled/yawed.
        # This gives a clean three-quarter perspective while keeping reset level.
        self._view_roll = math.radians(0)
        self._view_pitch = math.radians(70)
        self._view_yaw = math.radians(90)

        # Magnetometer heading is used as a *relative* yaw reference for this
        # playful viewer.  On startup/reset the current heading becomes yaw 0,
        # rather than making the model rotate toward magnetic north.
        self._mag_yaw_reference: float | None = None
        self._attitude_initialized = False

        self._last_update_ns: int | None = None
        self._valid = False
        self._stationary = False

        self._paint_timer = QTimer(self)
        self._paint_timer.timeout.connect(self._animate)
        self._paint_timer.start(33)

    @staticmethod
    def _wrap_pi(value: float) -> float:
        while value > math.pi:
            value -= 2.0 * math.pi
        while value < -math.pi:
            value += 2.0 * math.pi
        return value

    @classmethod
    def _blend_angle(cls, current: float, target: float, weight: float) -> float:
        error = cls._wrap_pi(target - current)
        return cls._wrap_pi(current + error * weight)

    @staticmethod
    def _sensor_to_model_frame(
        vector: tuple[float, float, float],
    ) -> tuple[float, float, float]:
        """Map the physical eval-board axes into the Korora model axes.

        The prototype mounting was experimentally aligned as:

            model X =  sensor Y
            model Y = -sensor X
            model Z =  sensor Z

        This is a real coordinate-frame rotation, so the same transform must be
        applied to accel, gyro, and magnetometer data *before* attitude
        estimation.  Swapping Euler angles only in the painter looks right near
        neutral but becomes inconsistent once multiple rotations are combined.
        """
        x, y, z = vector
        return y, -x, z

    def reset_pose(self) -> None:
        self._roll = 0.0
        self._pitch = 0.0
        self._yaw = 0.0
        self._display_roll = 0.0
        self._display_pitch = 0.0
        self._display_yaw = 0.0
        self._motion = [0.0, 0.0, 0.0]
        self._motion_velocity = [0.0, 0.0, 0.0]
        self._motion_accel = [0.0, 0.0, 0.0]
        self._mag_yaw_reference = None
        self._attitude_initialized = False
        self._last_animation_ns = time.monotonic_ns()
        self._last_update_ns = None
        self.update()

    def update_imu(
        self,
        accel_mg: tuple[int, int, int],
        gyro_mdps: tuple[int, int, int],
        mag_milligauss: tuple[int, int, int],
        received_ns: int,
        valid: bool,
    ) -> None:
        self._valid = valid
        if not valid:
            self._last_update_ns = None
            return

        first_update = self._last_update_ns is None
        if first_update:
            # Do not invent a 100 ms gyro integration step on the first packet.
            dt = 0.0
        else:
            dt = max(0.001, min(0.25, (received_ns - self._last_update_ns) / 1e9))
        self._last_update_ns = received_ns

        # Remap the actual sensor coordinate frame once, before doing any pose
        # maths.  This replaces the previous visual-only Euler swap:
        #     rotate(point, display_pitch, -display_roll, display_yaw)
        # which is not a valid frame transform when rotations combine.
        accel_model = self._sensor_to_model_frame(
            tuple(float(value) for value in accel_mg)
        )
        gyro_model = self._sensor_to_model_frame(
            tuple(float(value) for value in gyro_mdps)
        )
        mag_model = self._sensor_to_model_frame(
            tuple(float(value) for value in mag_milligauss)
        )

        ax, ay, az = (value / 1000.0 for value in accel_model)
        accel_norm = math.sqrt(ax * ax + ay * ay + az * az)
        gyro_dps = math.sqrt(
            sum((value / 1000.0) ** 2 for value in gyro_model))
        self._stationary = 0.85 <= accel_norm <= 1.15 and gyro_dps < 4.0

        accel_roll: float | None = None
        accel_pitch: float | None = None
        if accel_norm > 0.2 and 0.65 <= accel_norm <= 1.35:
            accel_roll = math.atan2(ay, az)
            accel_pitch = math.atan2(-ax, math.sqrt(ay * ay + az * az))

        # Initialize roll/pitch directly from gravity instead of starting at zero
        # and visibly sliding toward the correct stationary attitude.
        if not self._attitude_initialized:
            if accel_roll is not None and accel_pitch is not None:
                self._roll = accel_roll
                self._pitch = accel_pitch
            else:
                self._roll = 0.0
                self._pitch = 0.0
            self._yaw = 0.0
            self._attitude_initialized = True

        gx, gy, gz = (math.radians(value / 1000.0) for value in gyro_model)
        self._roll = self._wrap_pi(self._roll + gx * dt)
        self._pitch = self._wrap_pi(self._pitch + gy * dt)
        self._yaw = self._wrap_pi(self._yaw + gz * dt)

        if accel_roll is not None and accel_pitch is not None:
            correction = 0.055 if self._stationary else 0.018
            self._roll = self._blend_angle(self._roll, accel_roll, correction)
            self._pitch = self._blend_angle(
                self._pitch, accel_pitch, correction)

        mx, my, mz = mag_model
        mag_norm = math.sqrt(mx * mx + my * my + mz * mz)
        if mag_norm > 1.0:
            cr = math.cos(self._roll)
            sr = math.sin(self._roll)
            cp = math.cos(self._pitch)
            sp = math.sin(self._pitch)
            hx = mx * cp + mz * sp
            hy = mx * sr * sp + my * cr - mz * sr * cp
            magnetic_yaw = math.atan2(-hy, hx)

            # This GUI wants motion relative to however the user is holding the
            # board when the view starts/resets.  Capture that magnetic heading
            # as zero instead of converging the model to magnetic north (which
            # previously caused the visible run-up to about +95 degrees).
            if self._mag_yaw_reference is None:
                self._mag_yaw_reference = magnetic_yaw
                magnetic_yaw_relative = 0.0
            else:
                magnetic_yaw_relative = self._wrap_pi(
                    magnetic_yaw - self._mag_yaw_reference
                )

            self._yaw = self._blend_angle(
                self._yaw,
                magnetic_yaw_relative,
                0.018 if self._stationary else 0.006,
            )

        # Estimate just enough gravity-compensated acceleration to animate a
        # bounded "movement" effect.  This is intentionally not position
        # tracking: the GUI uses it as a spring impulse only.
        gravity_body = (
            -math.sin(self._pitch),
            math.sin(self._roll) * math.cos(self._pitch),
            math.cos(self._roll) * math.cos(self._pitch),
        )
        linear_body = (
            ax - gravity_body[0],
            ay - gravity_body[1],
            az - gravity_body[2],
        )
        linear_world = self._rotate(
            linear_body, self._roll, self._pitch, self._yaw
        )
        for index, value in enumerate(linear_world):
            # Kill tiny vibration/noise, then clamp violent spikes so a bumped
            # jumper wire cannot fling the model off screen.
            if abs(value) < 0.045:
                value = 0.0
            self._motion_accel[index] = max(-1.8, min(1.8, value))

    def _animate(self) -> None:
        self._display_roll = self._blend_angle(
            self._display_roll, self._roll, 0.22
        )
        self._display_pitch = self._blend_angle(
            self._display_pitch, self._pitch, 0.22
        )
        self._display_yaw = self._blend_angle(
            self._display_yaw, self._yaw, 0.22
        )

        now_ns = time.monotonic_ns()
        dt = max(0.005, min(0.08, (now_ns - self._last_animation_ns) / 1e9))
        self._last_animation_ns = now_ns

        # Damped spring toy model:
        #   measured linear accel -> short kick
        #   spring + drag          -> automatic return to center
        # The units below are deliberately visual rather than physical.
        """
        spring = 3.0
        damping = 5.0
        impulse_gain = 50.0
        for index in range(3):
            acceleration = (
                self._motion_accel[index] * impulse_gain
                - self._motion[index] * spring
                - self._motion_velocity[index] * damping
            )
            self._motion_velocity[index] += acceleration * dt
            self._motion[index] += self._motion_velocity[index] * dt
            self._motion[index] = max(-0.95, min(0.95, self._motion[index]))

        # The latest acceleration sample should behave like an impulse, not a
        # constant motor command between 10 Hz live telemetry packets.
        decay = math.exp(-8.0 * dt)
        self._motion_accel = [value * decay for value in self._motion_accel]
        """
        self.update()

    @staticmethod
    def _rotate(
        point: tuple[float, float, float], roll: float, pitch: float, yaw: float
    ) -> tuple[float, float, float]:
        x, y, z = point

        cr, sr = math.cos(roll), math.sin(roll)
        y, z = y * cr - z * sr, y * sr + z * cr

        cp, sp = math.cos(pitch), math.sin(pitch)
        x, z = x * cp + z * sp, -x * sp + z * cp

        cy, sy = math.cos(yaw), math.sin(yaw)
        x, y = x * cy - y * sy, x * sy + y * cy
        return x, y, z

    def _scene_transform(
        self, point: tuple[float, float, float]
    ) -> tuple[float, float, float]:
        # Sensor-to-model axis mapping is handled on the raw vectors in
        # update_imu().  Keep the renderer in the normal roll/pitch/yaw frame.
        posed = self._rotate(
            point,
            self._display_roll,
            self._display_pitch,
            self._display_yaw,
        )
        moved = (
            posed[0] + self._motion[0],
            posed[1] + self._motion[1],
            posed[2] + self._motion[2],
        )
        return self._rotate(
            moved, self._view_roll, self._view_pitch, self._view_yaw
        )

    @staticmethod
    def _project(
        point: tuple[float, float, float],
        cx: float,
        cy: float,
        scale: float,
    ) -> QPointF:
        x, y, z = point
        camera = 5.4
        depth = max(1.5, camera - z)
        perspective = camera / depth
        return QPointF(cx + x * scale * perspective, cy - y * scale * perspective)

    def paintEvent(self, event) -> None:  # type: ignore[override]
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)

        width = float(self.width())
        height = float(self.height())
        cx = width * 0.50
        cy = height * 0.50
        scale = max(34.0, min(width, height) * 0.28)

        painter.fillRect(self.rect(), QColor("#191522"))

        # Projected reference grid behind the board.  The large Korora front
        # face is an XY plane (constant Z), so keep the reference plane at a
        # constant Z as well.  Because both use the same fixed camera transform,
        # the grid is exactly parallel to the board front face at reset instead
        # of looking like a perpendicular floor.
        painter.setPen(QPen(QColor("#302a3d"), 1))
        grid_z = -1.25
        grid_extent = 3.2
        for step in range(-4, 5):
            offset = step * 0.72

            # Vertical grid line: X fixed, Y varies, Z fixed.
            a = self._rotate(
                (offset, -grid_extent, grid_z),
                self._view_roll, self._view_pitch, self._view_yaw,
            )
            b = self._rotate(
                (offset, grid_extent, grid_z),
                self._view_roll, self._view_pitch, self._view_yaw,
            )
            painter.drawLine(
                self._project(a, cx, cy, scale),
                self._project(b, cx, cy, scale),
            )

            # Horizontal grid line: Y fixed, X varies, Z fixed.
            a = self._rotate(
                (-grid_extent, offset, grid_z),
                self._view_roll, self._view_pitch, self._view_yaw,
            )
            b = self._rotate(
                (grid_extent, offset, grid_z),
                self._view_roll, self._view_pitch, self._view_yaw,
            )
            painter.drawLine(
                self._project(a, cx, cy, scale),
                self._project(b, cx, cy, scale),
            )

        vertices = [
            (-1.25, -0.72, -0.22),
            (1.25, -0.72, -0.22),
            (1.25, 0.72, -0.22),
            (-1.25, 0.72, -0.22),
            (-1.25, -0.72, 0.22),
            (1.25, -0.72, 0.22),
            (1.25, 0.72, 0.22),
            (-1.25, 0.72, 0.22),
        ]
        rotated = [self._scene_transform(point) for point in vertices]
        projected = [self._project(point, cx, cy, scale) for point in rotated]

        faces = [
            (0, 1, 2, 3),
            (4, 7, 6, 5),
            (0, 4, 5, 1),
            (1, 5, 6, 2),
            (2, 6, 7, 3),
            (3, 7, 4, 0),
        ]
        face_colors = [
            QColor("#3d3154"),
            QColor("#7857d6"),
            QColor("#51416e"),
            QColor("#604b83"),
            QColor("#493a63"),
            QColor("#362d48"),
        ]
        ordered_faces = sorted(
            enumerate(faces),
            key=lambda item: sum(rotated[index][2] for index in item[1]) / 4.0,
        )
        painter.setPen(QPen(QColor("#c9b7ff"), 1.4))
        for face_index, face in ordered_faces:
            painter.setBrush(face_colors[face_index])
            painter.drawPolygon(
                QPolygonF([projected[index] for index in face]))

        front_a = self._scene_transform((1.25, -0.30, 0.25))
        front_b = self._scene_transform((1.65, -0.30, 0.25))
        painter.setPen(QPen(QColor("#f0a6ca"), 4))
        painter.drawLine(
            self._project(front_a, cx, cy, scale),
            self._project(front_b, cx, cy, scale),
        )

        painter.setPen(QColor("#eeeaf8"))
        painter.drawText(12, 22, "LIVE IMU POSE")
        painter.setPen(QColor("#9b91ad"))
        status = "stationary correction" if self._stationary else "gyro tracking"
        if not self._valid:
            status = "waiting for IMU"
        motion_amount = math.sqrt(sum(value * value for value in self._motion))
        if self._valid and motion_amount > 0.035:
            status += " · motion spring"
        painter.drawText(12, 42, status)

        painter.setPen(QColor("#cebfff"))
        degrees = tuple(
            math.degrees(value)
            for value in (
                self._display_roll,
                self._display_pitch,
                self._display_yaw,
            )
        )
        painter.drawText(
            12,
            int(height - 14),
            f"roll {degrees[0]:+.1f}°   pitch {degrees[1]:+.1f}°   yaw {degrees[2]:+.1f}°",
        )


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


class LocalSensorsPanel(QFrame):
    status_led_requested = Signal(int, bool)

    def __init__(self) -> None:
        super().__init__()
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)

        title = QLabel("Korora local sensors")
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

    @staticmethod
    def _set_pill(label: QLabel, online: bool, text: str) -> None:
        label.setText(text)
        label.setProperty("online", online)
        label.style().unpolish(label)
        label.style().polish(label)

    def _reset_pose(self) -> None:
        self.pose.reset_pose()

    def update_state(self, state: LocalSensorState) -> None:
        self._set_pill(
            self.i2c_status, state.i2c_ready,
            "I²C ready" if state.i2c_ready else "I²C offline",
        )
        self._set_pill(
            self.imu_status, state.imu_present,
            "IMU online" if state.imu_present else "IMU missing",
        )
        self._set_pill(
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
