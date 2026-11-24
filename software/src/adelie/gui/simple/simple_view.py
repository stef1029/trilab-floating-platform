"""Simple mode: system health at a glance and plain controls per port."""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtWidgets import QGridLayout, QLabel, QPushButton, QVBoxLayout, QWidget

from ...controller import AdelieController
from ...health import BoardInfo, SystemHealth, configured_boards
from ..common import title_label
from .health_strip import HealthStrip
from .port_tile import PortTile

TILE_WIDTH = 360


class SimpleView(QWidget):
    board_setup_requested = Signal()

    def __init__(self, controller: AdelieController) -> None:
        super().__init__()
        self.controller = controller
        self.health = HealthStrip()
        self.tiles: dict[int, PortTile] = {}
        self._boards: tuple[BoardInfo, ...] = ()
        self._columns = 0

        self.no_ports = QLabel(
            "No ports are configured yet. Board setup is in Developer mode.")
        self.no_ports.setObjectName("muted")
        self.setup_button = QPushButton("Open board setup")
        self.setup_button.setObjectName("secondaryButton")
        self.setup_button.clicked.connect(self.board_setup_requested)

        self.grid = QGridLayout()
        self.grid.setSpacing(10)
        self.grid.setAlignment(Qt.AlignLeft | Qt.AlignTop)

        layout = QVBoxLayout(self)
        layout.addWidget(self.health)
        layout.addSpacing(6)
        layout.addWidget(title_label("Ports", "sectionTitle"))
        layout.addWidget(self.no_ports)
        layout.addWidget(self.setup_button, 0, Qt.AlignLeft)
        layout.addLayout(self.grid)
        layout.addStretch(1)

    def _rebuild_tiles(self, boards: tuple[BoardInfo, ...]) -> None:
        for tile in self.tiles.values():
            self.grid.removeWidget(tile)
            tile.deleteLater()
        self.tiles = {board.address: PortTile(board, self.controller) for board in boards}
        self._boards = boards
        self._relayout(force=True)
        self.no_ports.setVisible(not boards)
        self.setup_button.setVisible(not boards)

    def _relayout(self, force: bool = False) -> None:
        spacing = self.grid.spacing()
        columns = max(1, (self.width() + spacing) // (TILE_WIDTH + spacing))
        if columns == self._columns and not force:
            return
        self._columns = columns
        for index, board in enumerate(self._boards):
            tile = self.tiles[board.address]
            self.grid.removeWidget(tile)
            self.grid.addWidget(tile, index // columns, index % columns, Qt.AlignTop)

    def resizeEvent(self, event) -> None:  # type: ignore[no-untyped-def]  # noqa: N802
        super().resizeEvent(event)
        self._relayout()

    def refresh(self, health: SystemHealth) -> None:
        boards = tuple(configured_boards(self.controller.config))
        if boards != self._boards or not self.tiles and boards:
            self._rebuild_tiles(boards)
        self.health.update_health(health, self.controller.recorder.active)
        for board in boards:
            self.tiles[board.address].update_stats(
                self.controller.nodes.get(board.address), self.controller.connected
            )
