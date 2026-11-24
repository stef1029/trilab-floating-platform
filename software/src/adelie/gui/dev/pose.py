"""Live 3-D pose preview of the hub hub from its IMU."""

from __future__ import annotations

import math
import time

from PySide6.QtCore import QPointF, QTimer
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import (
    QSizePolicy,
    QWidget,
)


class LivePoseWidget(QWidget):
    """Lightweight 3-D-ish Hub pose toy driven by live IMU telemetry.

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
        # a zeroed Hub starts in a useful three-quarter perspective view.
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
        """Map the physical eval-board axes into the hub model axes.

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

        # Projected reference grid behind the board.  The large Hub front
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

