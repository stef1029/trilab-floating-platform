"""Board setup: map each discovered node UUID to a node index and label."""

from __future__ import annotations

from PySide6.QtWidgets import (
    QFrame,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
)

from ...config import BoardAssignment
from ...constants import DEFAULT_MAX_NODES
from ...controller import AdelieController
from ...models import InventoryEntry
from ..common import title_label


class BoardSetupPanel(QFrame):
    HELP = (
        "Each live node is listed by UUID. Press White LED to find a board, "
        "give it a node index (0-based) and an optional label, then save. "
        "When the saved UUID set matches the live boards exactly, the indices "
        "are applied to the hub automatically on every connect."
    )

    def __init__(self, controller: AdelieController) -> None:
        super().__init__()
        self.controller = controller
        self.entries: list[InventoryEntry] = []

        help_text = QLabel(self.HELP)
        help_text.setObjectName("muted")
        help_text.setWordWrap(True)
        self.status = QLabel("Connect to discover node boards")
        self.table = QTableWidget(0, 5)
        self.table.setMinimumHeight(220)
        self.table.setHorizontalHeaderLabels(
            ["UUID", "Address", "Node index", "Label", "Identify"]
        )
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        save = QPushButton("Save exact UUID configuration")
        save.clicked.connect(self.save)

        layout = QVBoxLayout(self)
        layout.addWidget(title_label("Board setup"))
        layout.addWidget(help_text)
        layout.addWidget(self.status)
        layout.addWidget(self.table, 1)
        layout.addWidget(save)

    def handle_event(self, kind: str, value: object) -> None:
        if kind == "inventory":
            self.entries = list(value["entries"])
            self._populate(bool(value["exact_match"]))
        elif kind == "inventory_applied":
            self.status.setText(
                "Exact UUID set matched and applied"
                if value
                else "UUID assignment was rejected. Review the complete configuration."
            )

    def _populate(self, exact_match: bool) -> None:
        self.table.setRowCount(len(self.entries))
        configured = {item.uuid: item for item in self.controller.config.assignments}
        for row, entry in enumerate(self.entries):
            self.table.setItem(row, 0, QTableWidgetItem(entry.uuid))
            self.table.setItem(row, 1, QTableWidgetItem(f"0x{entry.address:02x}"))
            assignment = configured.get(entry.uuid)
            number = QSpinBox()
            number.setRange(0, DEFAULT_MAX_NODES - 1)
            number.setValue(assignment.node_number if assignment else row)
            self.table.setCellWidget(row, 2, number)
            self.table.setCellWidget(
                row, 3, QLineEdit(assignment.label if assignment else ""))
            identify = QPushButton("White LED")
            identify.clicked.connect(
                lambda _checked=False, value=entry: self.controller.identify(value)
            )
            self.table.setCellWidget(row, 4, identify)
        self.status.setText(
            "Exact UUID set matched"
            if exact_match
            else "UUID set differs from the saved configuration. Configure every board."
        )

    def save(self) -> None:
        try:
            assignments = [
                BoardAssignment(
                    uuid=entry.uuid,
                    node_number=self.table.cellWidget(row, 2).value(),
                    label=self.table.cellWidget(row, 3).text(),
                )
                for row, entry in enumerate(self.entries)
            ]
            self.controller.save_assignments(assignments)
        except Exception as error:
            QMessageBox.critical(self, "Configuration error", str(error))
