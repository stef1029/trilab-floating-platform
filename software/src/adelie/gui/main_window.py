"""Main window: session bar on top, Simple or Developer view underneath."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings, QTimer, Slot
from PySide6.QtWidgets import QMainWindow, QStackedWidget, QVBoxLayout, QWidget

from ..constants import Opcode, Status
from ..controller import AdelieController
from ..health import HealthMonitor
from .common import UiBridge, vertical_scroll
from .dev.dev_view import DevView
from .session_bar import DEVELOPER, SIMPLE, SessionBar
from .simple.simple_view import SimpleView

REFRESH_MS = 250
CLOCK_EXCHANGE_MS = 1000
# Responses always reported in the status bar, success included, because the
# person pressing the button cannot otherwise tell whether the board acted.
REPORTED_OPCODES = (Opcode.ACTUATE_VALVE, Opcode.SET_RGB, Opcode.SET_AUDIO)


class MainWindow(QMainWindow):
    def __init__(
        self,
        config_path: Path,
        recording_directory: Path,
        initial_mode: int | None = None,
    ) -> None:
        super().__init__()
        self.setWindowTitle("Adelie")
        self.resize(1400, 900)
        self.settings = QSettings("trilab", "adelie")

        self.bridge = UiBridge()
        self.bridge.event.connect(self.handle_event)
        self.controller = AdelieController(config_path, self.bridge.event.emit)
        self.health_monitor = HealthMonitor()

        self.session_bar = SessionBar(self.controller, recording_directory)
        self.simple_view = SimpleView(self.controller)
        self.dev_view = DevView(self.controller)
        self.stack = QStackedWidget()
        # Simple mode scrolls as one page; Developer tabs scroll individually.
        self.stack.addWidget(vertical_scroll(self.simple_view))
        self.stack.addWidget(self.dev_view)

        root = QWidget()
        layout = QVBoxLayout(root)
        layout.addWidget(self.session_bar)
        layout.addWidget(self.stack, 1)
        self.setCentralWidget(root)

        self.session_bar.mode_changed.connect(self.set_mode)
        self.simple_view.board_setup_requested.connect(self._open_board_setup)
        mode = initial_mode
        if mode is None:
            mode = int(self.settings.value("mode", SIMPLE))
        self.set_mode(mode if mode in (SIMPLE, DEVELOPER) else SIMPLE)

        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self.refresh)
        self.refresh_timer.start(REFRESH_MS)
        self.clock_timer = QTimer(self)
        self.clock_timer.timeout.connect(self.controller.clock_exchange)
        self.clock_timer.start(CLOCK_EXCHANGE_MS)

    @Slot(int)
    def set_mode(self, mode: int) -> None:
        self.stack.setCurrentIndex(mode)
        self.session_bar.set_mode(mode)
        self.settings.setValue("mode", mode)

    def _open_board_setup(self) -> None:
        self.set_mode(DEVELOPER)
        self.dev_view.show_board_setup()

    @Slot(str, object)
    def handle_event(self, kind: str, value: object) -> None:
        self.session_bar.handle_event(kind, value)
        self.dev_view.handle_event(kind, value)
        if kind == "warning":
            self.statusBar().showMessage(str(value), 8000)
        elif kind == "recording" and not value["active"]:
            reason = value.get("reason", "")
            self.statusBar().showMessage(
                f"Recording saved to {value['path']}"
                + (f" ({reason})" if reason else ""), 12000)
        elif kind == "response":
            response = value["response"]
            failed = response.status not in (Status.OK, Status.ACCEPTED)
            if failed or response.opcode in REPORTED_OPCODES:
                self.statusBar().showMessage(
                    f"{response.opcode.name}: {response.status.name}", 8000)

    def refresh(self) -> None:
        diagnostics = self.controller.live_diagnostics()
        health = self.health_monitor.assess(self.controller, diagnostics)
        self.session_bar.refresh()
        # Both views keep their histories current; the hidden one is cheap.
        self.simple_view.refresh(health)
        self.dev_view.refresh(diagnostics)

    def closeEvent(self, event) -> None:  # type: ignore[no-untyped-def]  # noqa: N802
        self.controller.disconnect()
        event.accept()
