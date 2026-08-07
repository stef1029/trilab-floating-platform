from __future__ import annotations

import argparse
from datetime import datetime
from pathlib import Path
import sys
import time

from PySide6.QtCore import QObject, QTimer, Qt, Signal, Slot
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from .config import BoardAssignment
from .constants import (
    DEFAULT_MAX_FAIRIES,
    GALAPAGOS_ADDRESS,
    KORORA_ADDRESS,
    TelemetryLevel,
    is_assigned_fairy_address,
)
from .controller import AdelieController
from .models import InventoryEntry
from .widgets import NodeCard, TransportDiagnostics, TtlDiagnostics


STYLE = """
QWidget {
    background: #16131f;
    color: #eeeaf8;
    font-family: Inter, "Segoe UI", sans-serif;
    font-size: 13px;
}
QMainWindow { background: #120f1a; }
QFrame {
    background: #211c2d;
    border: 1px solid #3a3150;
    border-radius: 12px;
}
QGroupBox {
    border: 1px solid #44385e;
    border-radius: 8px;
    margin-top: 12px;
    padding-top: 8px;
    font-weight: 600;
}
QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; }
QPushButton {
    background: #7857d6;
    border: 0;
    border-radius: 7px;
    padding: 8px 12px;
    font-weight: 600;
}
QPushButton:hover { background: #9274e7; }
QPushButton:disabled { background: #4b4559; color: #8e879b; }
QPushButton#dangerButton { background: #b34b6c; }
QLineEdit, QSpinBox, QComboBox, QTableWidget {
    background: #191522;
    border: 1px solid #4b4065;
    border-radius: 6px;
    padding: 5px;
}
QLabel#nodeTitle { font-size: 19px; font-weight: 700; color: #cebfff; }
QLabel#statePill {
    background: #3a3345;
    border-radius: 8px;
    padding: 4px 8px;
}
QLabel#statePill[online="true"] { background: #245a48; color: #baf4dc; }
QLabel#lightGatePill {
    background: #302941;
    border-radius: 6px;
    padding: 3px 7px;
}
QLabel#lightGatePill[active="true"] {
    background: #c06a3d;
    color: #fff7ed;
    font-weight: 700;
}
"""


class UiBridge(QObject):
    event = Signal(str, object)


class MainWindow(QMainWindow):
    def __init__(self, config_path: Path, recording_directory: Path) -> None:
        super().__init__()
        self.setWindowTitle("Adelie")
        self.resize(1500, 900)
        self.bridge = UiBridge()
        self.bridge.event.connect(self.handle_event)
        self.controller = AdelieController(config_path, self.bridge.event.emit)
        self.recording_directory = recording_directory
        self.cards: dict[int, NodeCard] = {}
        self.inventory_entries: list[InventoryEntry] = []
        self._build_ui()

        self.refresh_timer = QTimer(self)
        self.refresh_timer.timeout.connect(self.refresh_stats)
        self.refresh_timer.start(250)
        self.clock_timer = QTimer(self)
        self.clock_timer.timeout.connect(self.controller.clock_exchange)
        self.clock_timer.start(1000)

    def _build_ui(self) -> None:
        root = QWidget()
        root_layout = QVBoxLayout(root)
        self.setCentralWidget(root)

        top = QHBoxLayout()
        self.connection_status = QLabel("Disconnected")
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.controller.connect)
        self.disconnect_button = QPushButton("Disconnect")
        self.disconnect_button.clicked.connect(self.controller.disconnect)
        self.disconnect_button.setEnabled(False)
        top.addWidget(self.connection_status)
        top.addStretch()
        top.addWidget(self.connect_button)
        top.addWidget(self.disconnect_button)
        root_layout.addLayout(top)

        run = QHBoxLayout()
        default_name = datetime.now().strftime("run_%Y%m%d_%H%M%S.log")
        self.log_path = QLineEdit(
            str(recording_directory_path(self.recording_directory, default_name)))
        browse = QPushButton("Choose log")
        browse.clicked.connect(self.choose_log)
        self.record_button = QPushButton("Record Start")
        self.record_button.clicked.connect(self.start_recording)
        self.record_button.setEnabled(False)
        self.stop_button = QPushButton("Record Stop")
        self.stop_button.clicked.connect(self.controller.stop_recording)
        self.stop_button.setEnabled(False)
        run.addWidget(QLabel("Run log"))
        run.addWidget(self.log_path, 1)
        run.addWidget(browse)
        run.addWidget(self.record_button)
        run.addWidget(self.stop_button)
        root_layout.addLayout(run)

        modes = QHBoxLayout()
        self.telemetry = QComboBox()
        self.telemetry.addItems(["Critical", "Standard", "Full"])
        self.telemetry.setCurrentIndex(int(TelemetryLevel.STANDARD))
        self.telemetry.currentIndexChanged.connect(
            lambda index: self.controller.set_telemetry(TelemetryLevel(index))
        )
        self.sync_test = QPushButton("Test sync")
        self.sync_test.clicked.connect(self.controller.start_sync_test)
        self.stop_sync_test = QPushButton("Stop sync test")
        self.stop_sync_test.clicked.connect(self.controller.stop_sync_test)
        self.ttl_frequency = QDoubleSpinBox()
        self.ttl_frequency.setRange(0.1, 10.0)
        self.ttl_frequency.setDecimals(1)
        self.ttl_frequency.setSingleStep(0.1)
        self.ttl_frequency.setValue(1.0)
        self.ttl_width = QSpinBox()
        self.ttl_width.setRange(1, 2_000_000)
        self.ttl_width.setValue(100)
        start_ttl = QPushButton("Start TTL")
        start_ttl.clicked.connect(
            lambda: self.controller.start_ttl(
                float(self.ttl_frequency.value()), self.ttl_width.value()
            )
        )
        stop_ttl = QPushButton("Stop TTL")
        stop_ttl.clicked.connect(self.controller.stop_ttl)
        modes.addWidget(QLabel("Telemetry"))
        modes.addWidget(self.telemetry)
        modes.addWidget(self.sync_test)
        modes.addWidget(self.stop_sync_test)
        modes.addStretch()
        modes.addWidget(QLabel("TTL Hz"))
        modes.addWidget(self.ttl_frequency)
        modes.addWidget(QLabel("Width Âµs"))
        modes.addWidget(self.ttl_width)
        modes.addWidget(start_ttl)
        modes.addWidget(stop_ttl)
        root_layout.addLayout(modes)

        diagnostics = QHBoxLayout()
        diagnostics.setContentsMargins(0, 0, 0, 0)
        self.ttl_diagnostics = TtlDiagnostics()
        self.transport_diagnostics = TransportDiagnostics()
        diagnostics.addWidget(self.ttl_diagnostics, 1)
        diagnostics.addWidget(self.transport_diagnostics, 2)
        root_layout.addLayout(diagnostics)

        self.inventory_status = QLabel("Connect to discover Fairy boards")
        root_layout.addWidget(self.inventory_status)
        self.inventory_table = QTableWidget(0, 5)
        self.inventory_table.setFixedHeight(110)
        self.inventory_table.setHorizontalHeaderLabels(
            ["UUID", "Address", "Fairy index", "Label", "Identify"]
        )
        self.inventory_table.horizontalHeader().setSectionResizeMode(
            0, QHeaderView.Stretch
        )
        save_inventory = QPushButton("Save exact UUID configuration")
        save_inventory.clicked.connect(self.save_inventory)
        root_layout.addWidget(self.inventory_table)
        root_layout.addWidget(save_inventory)

        self.card_container = QWidget()
        self.card_layout = QHBoxLayout(self.card_container)
        self.card_layout.setAlignment(Qt.AlignLeft | Qt.AlignTop)
        self.card_layout.addStretch()
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self.card_container)
        root_layout.addWidget(scroll, 1)

    def ensure_card(self, address: int, name: str) -> NodeCard:
        card = self.cards.get(address)
        if card is not None:
            return card
        card = NodeCard(address, name)
        card.setMinimumWidth(290)
        card.rgb_requested.connect(self.controller.set_rgb)
        card.ir_requested.connect(self.controller.set_ir)
        card.audio_requested.connect(
            lambda node, mode, frequency, low, high, amplitude, duration:
            self.controller.set_audio(
                node,
                mode=mode,
                frequency_hz=frequency,
                low_hz=low,
                high_hz=high,
                amplitude=amplitude,
                duration_ms=duration,
            )
        )
        card.valve_requested.connect(self.controller.valve)
        self.card_layout.insertWidget(
            self.card_layout.count() - 1, card, 0, Qt.AlignTop
        )
        self.cards[address] = card
        return card

    @Slot(str, object)
    def handle_event(self, kind: str, value: object) -> None:
        if kind == "connection":
            state = value["state"]
            self.connection_status.setText(f"{state}: {value['detail']}")
            connected = state == "connected"
            self.connect_button.setEnabled(not connected)
            self.disconnect_button.setEnabled(connected)
        elif kind == "inventory":
            self.inventory_entries = list(value["entries"])
            self.populate_inventory(bool(value["exact_match"]))
        elif kind == "inventory_applied":
            applied = bool(value)
            self.inventory_status.setText(
                "Exact UUID set matched and applied"
                if applied
                else "UUID assignment was rejected. Review the complete configuration."
            )
            self.record_button.setEnabled(False)
            if applied:
                self.controller.configure_valves()
        elif kind == "recording":
            active = bool(value["active"])
            self.record_button.setEnabled(
                not active and self.controller.inventory_applied)
            self.stop_button.setEnabled(active)
            if not active:
                self.log_path.setText(
                    str(
                        recording_directory_path(
                            self.recording_directory,
                            datetime.now().strftime("run_%Y%m%d_%H%M%S.log"),
                        )
                    )
                )
        elif kind == "warning":
            self.statusBar().showMessage(str(value), 8000)
        elif kind == "ttl":
            self.ttl_diagnostics.update_ttl(value)
        elif kind == "response" and value["response"].status not in (
            0,
            1,
        ):
            response = value["response"]
            self.statusBar().showMessage(
                f"{response.opcode.name}: {response.status.name}", 8000
            )

    def populate_inventory(self, exact_match: bool) -> None:
        self.inventory_table.setRowCount(len(self.inventory_entries))
        configured_by_uuid = {
            item.uuid: item for item in self.controller.config.assignments
        }
        for row, entry in enumerate(self.inventory_entries):
            self.inventory_table.setItem(row, 0, QTableWidgetItem(entry.uuid))
            self.inventory_table.setItem(
                row, 1, QTableWidgetItem(f"0x{entry.address:02x}")
            )
            number = QSpinBox()
            number.setRange(0, DEFAULT_MAX_FAIRIES - 1)
            assignment = configured_by_uuid.get(entry.uuid)
            number.setValue(assignment.fairy_number if assignment else row)
            self.inventory_table.setCellWidget(row, 2, number)
            label = QLineEdit(assignment.label if assignment else "")
            self.inventory_table.setCellWidget(row, 3, label)
            identify = QPushButton("White LED")
            identify.clicked.connect(
                lambda _checked=False, value=entry: self.controller.identify(
                    value)
            )
            self.inventory_table.setCellWidget(row, 4, identify)
        self.inventory_status.setText(
            "Exact UUID set matched"
            if exact_match
            else "UUID set differs from the saved configuration. Configure every board."
        )
        self.record_button.setEnabled(False)

    def save_inventory(self) -> None:
        try:
            assignments: list[BoardAssignment] = []
            for row, entry in enumerate(self.inventory_entries):
                number = self.inventory_table.cellWidget(row, 2)
                label = self.inventory_table.cellWidget(row, 3)
                assignments.append(
                    BoardAssignment(
                        uuid=entry.uuid,
                        fairy_number=number.value(),
                        label=label.text(),
                    )
                )
            self.controller.save_assignments(assignments)
        except Exception as error:
            QMessageBox.critical(self, "Configuration error", str(error))

    def choose_log(self) -> None:
        selected, _ = QFileDialog.getSaveFileName(
            self, "Choose run log", self.log_path.text(), "Fairy log (*.log)"
        )
        if selected:
            self.log_path.setText(selected if selected.endswith(
                ".log") else selected + ".log")

    def start_recording(self) -> None:
        try:
            self.controller.start_recording(Path(self.log_path.text()))
        except Exception as error:
            QMessageBox.critical(self, "Cannot start recording", str(error))

    def refresh_stats(self) -> None:
        diagnostics = self.controller.live_diagnostics()
        visible_nodes = {
            address: stats
            for address, stats in self.controller.nodes.items()
            if address in (KORORA_ADDRESS, GALAPAGOS_ADDRESS)
            or is_assigned_fairy_address(address)
        }
        live_addresses = set(visible_nodes)
        for address in set(self.cards) - live_addresses:
            card = self.cards.pop(address)
            self.card_layout.removeWidget(card)
            card.deleteLater()
        for address, stats in visible_nodes.items():
            card = self.ensure_card(address, stats.name)
            card.update_stats(stats)
            if address == KORORA_ADDRESS:
                card.update_clock_diagnostics(diagnostics)
        self.transport_diagnostics.update_diagnostics(diagnostics)
        self.ttl_diagnostics.update_diagnostics(diagnostics)
        if not self.controller.recorder.active:
            self.record_button.setEnabled(self.controller.ready_to_record)

    def closeEvent(self, event) -> None:  # type: ignore[no-untyped-def]
        self.controller.disconnect()
        event.accept()


def recording_directory_path(directory: Path, name: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    if not path.exists():
        return path
    stem = path.stem
    for index in range(1, 1000):
        candidate = path.with_name(f"{stem}_{index:03d}.log")
        if not candidate.exists():
            return candidate
    raise RuntimeError("could not allocate a new log filename")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Adelie control application")
    parser.add_argument("--config", type=Path,
                        default=Path("config/boards.json"))
    parser.add_argument("--recordings", type=Path, default=Path("recordings"))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    application = QApplication(sys.argv)
    application.setStyleSheet(STYLE)
    window = MainWindow(args.config, args.recordings)
    window.show()
    sys.exit(application.exec())


if __name__ == "__main__":
    main()
