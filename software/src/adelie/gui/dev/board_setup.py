"""Board setup: which node numbers this rig uses, and what each port is called."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QFrame,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from ...config import BoardAssignment
from ...constants import DEFAULT_MAX_NODES, NODE_ADDRESS_BASE
from ...controller import AdelieController
from ..common import title_label

COLUMN_NODE, COLUMN_STATUS, COLUMN_UUID, COLUMN_USED, COLUMN_LABEL, COLUMN_IDENTIFY = range(6)


class BoardSetupPanel(QFrame):
    HELP = (
        "Each board's node number is set when it is flashed "
        "(firmware/reward_port/scripts/flash.py --id N) and is written on the "
        "board. Tick the nodes this rig uses: every ticked node must be "
        "connected and synchronised before a recording can start. Press White "
        "LED to find a board, give its port a label, then save."
    )

    def __init__(self, controller: AdelieController) -> None:
        super().__init__()
        self.controller = controller
        self.present: dict[int, object] = {}

        help_text = QLabel(self.HELP)
        help_text.setObjectName("muted")
        help_text.setWordWrap(True)
        self.status = QLabel("Connect to the hub to see which boards are present")
        self.table = QTableWidget(DEFAULT_MAX_NODES, 6)
        self.table.setMinimumHeight(220)
        self.table.setHorizontalHeaderLabels(
            ["Node", "Status", "UUID", "In use", "Label", "Identify"]
        )
        self.table.verticalHeader().setVisible(False)
        self.table.horizontalHeader().setSectionResizeMode(
            COLUMN_UUID, QHeaderView.Stretch)

        self.used: list[QCheckBox] = []
        self.labels: list[QLineEdit] = []
        self.identify_buttons: list[QPushButton] = []
        for number in range(DEFAULT_MAX_NODES):
            node = QTableWidgetItem(f"{number}  (0x{NODE_ADDRESS_BASE + number:02x})")
            node.setFlags(node.flags() & ~Qt.ItemIsEditable)
            self.table.setItem(number, COLUMN_NODE, node)
            used = QCheckBox()
            self.used.append(used)
            self.table.setCellWidget(number, COLUMN_USED, used)
            label = QLineEdit()
            self.labels.append(label)
            self.table.setCellWidget(number, COLUMN_LABEL, label)
            identify = QPushButton("White LED")
            identify.clicked.connect(
                lambda _checked=False, value=number: self._identify(value))
            self.identify_buttons.append(identify)
            self.table.setCellWidget(number, COLUMN_IDENTIFY, identify)

        save = QPushButton("Save board configuration")
        save.clicked.connect(self.save)

        layout = QVBoxLayout(self)
        layout.addWidget(title_label("Board setup"))
        layout.addWidget(help_text)
        layout.addWidget(self.status)
        layout.addWidget(self.table, 1)
        layout.addWidget(save)

        self._load_config()
        self._show_presence([], [])

    def handle_event(self, kind: str, value: object) -> None:
        if kind == "inventory":
            self.present = {entry.node_number: entry for entry in value["entries"]}
            self._show_presence(value["missing"], value["unexpected"])

    def _load_config(self) -> None:
        configured = {
            item.node_number: item for item in self.controller.config.assignments
        }
        for number in range(DEFAULT_MAX_NODES):
            assignment = configured.get(number)
            self.used[number].setChecked(assignment is not None)
            self.labels[number].setText(assignment.label if assignment else "")

    def _show_presence(self, missing: list[int], unexpected: list[int]) -> None:
        for number in range(DEFAULT_MAX_NODES):
            entry = self.present.get(number)
            status = "Present" if entry is not None else "Not found"
            uuid = (entry.uuid or "not reported yet") if entry is not None else ""
            for column, text in ((COLUMN_STATUS, status), (COLUMN_UUID, uuid)):
                item = QTableWidgetItem(text)
                item.setFlags(item.flags() & ~Qt.ItemIsEditable)
                self.table.setItem(number, column, item)
            self.identify_buttons[number].setEnabled(entry is not None)
        if not self.controller.connected:
            self.status.setText("Connect to the hub to see which boards are present")
            return
        problems = []
        if missing:
            problems.append(
                "in use but not found: " + ", ".join(str(n) for n in missing))
        if unexpected:
            problems.append(
                "found but not in use: " + ", ".join(str(n) for n in unexpected))
        self.status.setText(
            "; ".join(problems).capitalize()
            if problems
            else f"All {len(self.controller.config.assignments)} boards in use are present"
        )

    def _identify(self, number: int) -> None:
        entry = self.present.get(number)
        if entry is not None:
            self.controller.identify(entry)

    def save(self) -> None:
        try:
            assignments = [
                BoardAssignment(
                    node_number=number,
                    label=self.labels[number].text(),
                )
                for number in range(DEFAULT_MAX_NODES)
                if self.used[number].isChecked()
            ]
            self.controller.save_assignments(assignments)
        except Exception as error:
            QMessageBox.critical(self, "Configuration error", str(error))
