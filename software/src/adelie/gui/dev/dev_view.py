"""Developer mode: every diagnostic and raw control, organised into tabs."""

from __future__ import annotations

from PySide6.QtWidgets import (
    QComboBox,
    QDoubleSpinBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from ...constants import TelemetryLevel
from ...controller import AdelieController
from ..common import vertical_scroll
from .board_setup import BoardSetupPanel
from .boards_tab import BoardsTab
from .link_panel import TransportDiagnostics
from .sensors_panel import LocalSensorsPanel
from .serial_console import SerialConsole
from .ttl_panel import TtlDiagnostics


class TtlControls(QFrame):
    """Telemetry level, sync test and scheduled TTL train controls."""

    def __init__(self, controller: AdelieController) -> None:
        super().__init__()
        self.telemetry = QComboBox()
        self.telemetry.addItems(["Critical", "Standard", "Full"])
        self.telemetry.setCurrentIndex(int(controller.telemetry_level))
        self.telemetry.currentIndexChanged.connect(
            lambda index: controller.set_telemetry(TelemetryLevel(index))
        )
        sync_test = QPushButton("Start sync test")
        sync_test.setToolTip(
            "Full telemetry, safe random RGB commands and a 1 Hz TTL train")
        sync_test.clicked.connect(controller.start_sync_test)
        stop_sync_test = QPushButton("Stop sync test")
        stop_sync_test.setObjectName("secondaryButton")
        stop_sync_test.clicked.connect(controller.stop_sync_test)

        self.ttl_frequency = QDoubleSpinBox()
        self.ttl_frequency.setRange(0.1, 10.0)
        self.ttl_frequency.setDecimals(1)
        self.ttl_frequency.setSingleStep(0.1)
        self.ttl_frequency.setValue(1.0)
        self.ttl_frequency.setSuffix(" Hz")
        self.ttl_width = QSpinBox()
        self.ttl_width.setRange(1, 2_000_000)
        self.ttl_width.setValue(100)
        self.ttl_width.setSuffix(" µs")
        start_ttl = QPushButton("Start TTL")
        start_ttl.clicked.connect(
            lambda: controller.start_ttl(
                float(self.ttl_frequency.value()), self.ttl_width.value()
            )
        )
        stop_ttl = QPushButton("Stop TTL")
        stop_ttl.setObjectName("secondaryButton")
        stop_ttl.clicked.connect(controller.stop_ttl)

        row = QHBoxLayout(self)
        row.addWidget(QLabel("Telemetry"))
        row.addWidget(self.telemetry)
        row.addWidget(sync_test)
        row.addWidget(stop_sync_test)
        row.addStretch()
        row.addWidget(QLabel("TTL train"))
        row.addWidget(self.ttl_frequency)
        row.addWidget(QLabel("width"))
        row.addWidget(self.ttl_width)
        row.addWidget(start_ttl)
        row.addWidget(stop_ttl)


def _page(*widgets: QWidget, stretch_last: bool = True) -> QWidget:
    page = QWidget()
    layout = QVBoxLayout(page)
    for widget in widgets:
        layout.addWidget(widget)
    if stretch_last:
        layout.addStretch(1)
    return vertical_scroll(page)


class DevView(QWidget):
    TAB_BOARDS = 0
    TAB_SETUP = 4

    def __init__(self, controller: AdelieController) -> None:
        super().__init__()
        self.controller = controller

        self.boards = BoardsTab(controller)
        self.transport = TransportDiagnostics()
        self.ttl_controls = TtlControls(controller)
        self.ttl = TtlDiagnostics()
        self.sensors = LocalSensorsPanel()
        self.sensors.status_led_requested.connect(controller.set_status_led)
        self.board_setup = BoardSetupPanel(controller)
        self.serial = SerialConsole()

        self.tabs = QTabWidget()
        self.tabs.addTab(vertical_scroll(self.boards), "Boards")
        self.tabs.addTab(_page(self.transport), "Link && clock")
        self.tabs.addTab(_page(self.ttl_controls, self.ttl), "TTL && sync")
        self.tabs.addTab(_page(self.sensors), "Hub sensors && power")
        self.tabs.addTab(_page(self.board_setup, stretch_last=False), "Board setup")
        self.tabs.addTab(self.serial, "Hub serial")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.tabs, 1)

    def show_board_setup(self) -> None:
        self.tabs.setCurrentIndex(self.TAB_SETUP)

    def handle_event(self, kind: str, value: object) -> None:
        if kind == "inventory":
            self.board_setup.handle_event(kind, value)
        elif kind == "ttl":
            self.ttl.update_ttl(value)
        elif kind == "local_sensors":
            self.sensors.update_state(value)

    def refresh(self, diagnostics: dict[str, object]) -> None:
        self.boards.refresh(diagnostics)
        self.transport.update_diagnostics(diagnostics)
        self.ttl.update_diagnostics(diagnostics)
        self.sensors.update_state(self.controller.local_sensors)
