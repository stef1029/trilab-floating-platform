"""One tile per configured node port: status, beam, reward, light and sound."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
)

from ...controller import AdelieController
from ...health import BoardInfo, NodeState, beam_active, node_state
from ...models import NodeStats
from ..common import repolish, set_pill

# (name, (r, g, b)) presets.  Brightness scales these at send time.
LIGHT_PRESETS = (
    ("Off", (0, 0, 0)),
    ("Red", (255, 0, 0)),
    ("Green", (0, 255, 0)),
    ("Blue", (0, 0, 255)),
    ("White", (255, 255, 255)),
)
AUDIO_OFF, AUDIO_TONE, AUDIO_NOISE = 0, 1, 2
NOISE_BAND_HZ = (8_000, 16_000)
MAX_AMPLITUDE = 2047

_STATE_TEXT = {
    NodeState.READY: "ready",
    NodeState.ONLINE: "syncing",
    NodeState.OFFLINE: "offline",
}


class PortTile(QFrame):
    def __init__(self, board: BoardInfo, controller: AdelieController) -> None:
        super().__init__()
        self.board = board
        self.controller = controller
        self.setMinimumWidth(330)

        name = QLabel(board.name)
        name.setObjectName("nodeTitle")
        self.state = QLabel("offline")
        self.state.setObjectName("statePill")
        header = QHBoxLayout()
        header.addWidget(name)
        header.addStretch()
        header.addWidget(self.state)

        self.beam = QLabel("Beam clear")
        self.beam.setObjectName("lightGatePill")
        self.beam.setProperty("active", False)

        grid = QGridLayout()
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(10)
        grid.setColumnStretch(1, 1)

        # Reward (valve)
        self.valve_ms = QSpinBox()
        self.valve_ms.setRange(1, 250)
        self.valve_ms.setValue(20)
        self.valve_ms.setSuffix(" ms")
        self.reward_button = QPushButton("Give reward")
        self.reward_button.setObjectName("dangerButton")
        self.reward_button.clicked.connect(
            lambda: controller.valve(board.address, self.valve_ms.value())
        )
        reward = QHBoxLayout()
        reward.addWidget(self.valve_ms)
        reward.addWidget(self.reward_button, 1)
        grid.addWidget(QLabel("Reward"), 0, 0)
        grid.addLayout(reward, 0, 1)

        # Light (RGB presets + brightness)
        self.brightness = QSpinBox()
        self.brightness.setRange(1, 100)
        self.brightness.setValue(100)
        self.brightness.setSuffix(" %")
        self.brightness.setToolTip("Brightness for the colour buttons")
        light = QHBoxLayout()
        light.setSpacing(6)
        self.light_buttons: list[QPushButton] = []
        for label, rgb in LIGHT_PRESETS:
            button = QPushButton("" if label != "Off" else "✕")
            button.setObjectName("swatch")
            button.setToolTip(label)
            colour = "#191522" if label == "Off" else "rgb({}, {}, {})".format(*rgb)
            button.setStyleSheet(f"background: {colour};")
            button.clicked.connect(lambda _checked=False, value=rgb: self._set_light(value))
            light.addWidget(button)
            self.light_buttons.append(button)
        light.addStretch()
        light.addWidget(self.brightness)
        grid.addWidget(QLabel("Light"), 1, 0)
        grid.addLayout(light, 1, 1)

        # Sound (tone or noise)
        self.sound_kind = QComboBox()
        self.sound_kind.addItems(["Tone", "Noise"])
        self.tone_hz = QSpinBox()
        self.tone_hz.setRange(100, 40_000)
        self.tone_hz.setSingleStep(500)
        self.tone_hz.setValue(10_000)
        self.tone_hz.setSuffix(" Hz")
        self.sound_kind.currentIndexChanged.connect(
            lambda index: self.tone_hz.setEnabled(index == 0)
        )
        self.sound_ms = QSpinBox()
        self.sound_ms.setRange(10, 60_000)
        self.sound_ms.setSingleStep(100)
        self.sound_ms.setValue(500)
        self.sound_ms.setSuffix(" ms")
        self.volume = QSpinBox()
        self.volume.setRange(1, 100)
        self.volume.setValue(25)
        self.volume.setSuffix(" %")
        self.volume.setToolTip("Volume")
        self.play_button = QPushButton("Play")
        self.play_button.clicked.connect(self._play)
        stop = QPushButton("Stop")
        stop.setObjectName("secondaryButton")
        stop.clicked.connect(self._stop_sound)
        self.stop_button = stop
        sound_top = QHBoxLayout()
        sound_top.addWidget(self.sound_kind)
        sound_top.addWidget(self.tone_hz, 1)
        sound_top.addWidget(self.volume)
        sound_bottom = QHBoxLayout()
        sound_bottom.addWidget(self.sound_ms)
        sound_bottom.addWidget(self.play_button, 1)
        sound_bottom.addWidget(stop)
        sound = QVBoxLayout()
        sound.setSpacing(6)
        sound.addLayout(sound_top)
        sound.addLayout(sound_bottom)
        grid.addWidget(QLabel("Sound"), 2, 0, Qt.AlignTop)
        grid.addLayout(sound, 2, 1)

        layout = QVBoxLayout(self)
        layout.setSpacing(10)
        layout.addLayout(header)
        layout.addWidget(self.beam)
        layout.addLayout(grid)
        self._controls = [
            self.reward_button, self.play_button, self.stop_button,
            *self.light_buttons,
        ]

    def _set_light(self, rgb: tuple[int, int, int]) -> None:
        scale = self.brightness.value() / 100.0
        red, green, blue = (round(value * scale) for value in rgb)
        self.controller.set_rgb(self.board.address, red, green, blue)

    def _play(self) -> None:
        noise = self.sound_kind.currentIndex() == 1
        low, high = NOISE_BAND_HZ
        self.controller.set_audio(
            self.board.address,
            mode=AUDIO_NOISE if noise else AUDIO_TONE,
            frequency_hz=self.tone_hz.value(),
            low_hz=low,
            high_hz=high,
            amplitude=round(MAX_AMPLITUDE * self.volume.value() / 100),
            duration_ms=self.sound_ms.value(),
        )

    def _stop_sound(self) -> None:
        low, high = NOISE_BAND_HZ
        self.controller.set_audio(
            self.board.address, mode=AUDIO_OFF, frequency_hz=self.tone_hz.value(),
            low_hz=low, high_hz=high, amplitude=0, duration_ms=0,
        )

    def update_stats(self, stats: NodeStats | None, hub_connected: bool) -> None:
        state = node_state(stats)
        set_pill(self.state, state is not NodeState.OFFLINE, _STATE_TEXT[state])
        for control in self._controls:
            control.setEnabled(hub_connected and state is not NodeState.OFFLINE)

        active = beam_active(stats)
        count = stats.light_gate_events if stats is not None else 0
        if active:
            text = f"BEAM BREAK · {count}"
        elif count:
            text = f"Beam clear · {count} breaks"
        else:
            text = "Beam clear"
        self.beam.setText(text)
        self.beam.setProperty("active", active)
        repolish(self.beam)
