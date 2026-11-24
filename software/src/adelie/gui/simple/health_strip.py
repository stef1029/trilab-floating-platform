"""A row of colour-coded health tiles plus the ready-to-record banner."""

from __future__ import annotations

from PySide6.QtWidgets import QFrame, QGridLayout, QLabel, QVBoxLayout, QWidget

from ...health import Level, SystemHealth
from ..common import set_level_style

_SYMBOL = {
    Level.UNKNOWN: "○",
    Level.OK: "●",
    Level.WARN: "▲",
    Level.ERROR: "■",
}


class HealthTile(QFrame):
    def __init__(self, title: str) -> None:
        super().__init__()
        self.title = QLabel(title)
        self.title.setObjectName("muted")
        self.value = QLabel("-")
        self.value.setWordWrap(True)
        self.value.setStyleSheet("font-size: 14px; font-weight: 600;")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 8, 12, 10)
        layout.setSpacing(2)
        layout.addWidget(self.title)
        layout.addWidget(self.value)

    def set_item(self, level: Level, detail: str) -> None:
        self.value.setText(f"{_SYMBOL[level]}  {detail}")
        set_level_style(
            self.value, level, "border-radius: 6px; padding: 4px 8px;"
            " font-size: 14px; font-weight: 600;"
        )


class HealthStrip(QWidget):
    MIN_TILE_WIDTH = 210

    def __init__(self) -> None:
        super().__init__()
        self.tiles: dict[str, HealthTile] = {}
        self._columns = 0
        self.grid = QGridLayout()
        self.grid.setSpacing(8)
        self.banner = QLabel("Connect to the hub to start")
        self.banner.setObjectName("readyBanner")
        set_level_style(self.banner, Level.UNKNOWN)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.banner)
        layout.addLayout(self.grid)

    def _relayout(self, force: bool = False) -> None:
        count = max(1, len(self.tiles))
        columns = max(1, min(count, self.width() // self.MIN_TILE_WIDTH))
        if columns == self._columns and not force:
            return
        self._columns = columns
        for index, tile in enumerate(self.tiles.values()):
            self.grid.removeWidget(tile)
            self.grid.addWidget(tile, index // columns, index % columns)

    def resizeEvent(self, event) -> None:  # type: ignore[no-untyped-def]  # noqa: N802
        super().resizeEvent(event)
        self._relayout()

    def update_health(self, health: SystemHealth, recording: bool) -> None:
        for item in health.items:
            tile = self.tiles.get(item.key)
            if tile is None:
                tile = HealthTile(item.title)
                self.tiles[item.key] = tile
                self._relayout(force=True)
            tile.set_item(item.level, item.detail)

        if recording:
            text, level = "●  Recording", Level.ERROR
        elif health.ready_to_record:
            text, level = "✓  Ready to record", Level.OK
        else:
            level = Level.WARN if health.item("hub").level is Level.OK else Level.UNKNOWN
            text = health.ready_reason
        self.banner.setText(text)
        set_level_style(self.banner, level)
