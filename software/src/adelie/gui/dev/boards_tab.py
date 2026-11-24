"""Boards tab: one NodeCard per live node, wrapped into a responsive grid."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QGridLayout, QWidget

from ...constants import TTL_GENERATOR_ADDRESS, HUB_ADDRESS, is_assigned_node_address
from ...controller import AdelieController
from .node_card import NodeCard

CARD_WIDTH = 300


class BoardsTab(QWidget):
    def __init__(self, controller: AdelieController) -> None:
        super().__init__()
        self.controller = controller
        self.cards: dict[int, NodeCard] = {}
        self._columns = 0
        self.grid = QGridLayout(self)
        self.grid.setAlignment(Qt.AlignLeft | Qt.AlignTop)
        self.grid.setSpacing(10)

    def _ensure_card(self, address: int, name: str) -> NodeCard:
        card = self.cards.get(address)
        if card is not None:
            return card
        card = NodeCard(address, name)
        card.setFixedWidth(CARD_WIDTH)
        controller = self.controller
        card.rgb_requested.connect(controller.set_rgb)
        card.ir_requested.connect(controller.set_ir)
        card.audio_requested.connect(
            lambda node, mode, frequency, low, high, amplitude, duration:
            controller.set_audio(
                node,
                mode=mode,
                frequency_hz=frequency,
                low_hz=low,
                high_hz=high,
                amplitude=amplitude,
                duration_ms=duration,
            )
        )
        card.valve_requested.connect(controller.valve)
        self.cards[address] = card
        self._relayout(force=True)
        return card

    def _relayout(self, force: bool = False) -> None:
        spacing = self.grid.spacing()
        columns = max(1, (self.width() + spacing) // (CARD_WIDTH + spacing))
        if columns == self._columns and not force:
            return
        self._columns = columns
        for card in self.cards.values():
            self.grid.removeWidget(card)
        # Hub, TTL generator, then Nodes in address order.
        for index, address in enumerate(sorted(self.cards)):
            self.grid.addWidget(
                self.cards[address], index // columns, index % columns, Qt.AlignTop
            )

    def resizeEvent(self, event) -> None:  # type: ignore[no-untyped-def]  # noqa: N802
        super().resizeEvent(event)
        self._relayout()

    def refresh(self, diagnostics: dict[str, object]) -> None:
        visible = {
            address: stats
            for address, stats in self.controller.nodes.items()
            if address in (HUB_ADDRESS, TTL_GENERATOR_ADDRESS)
            or is_assigned_node_address(address)
        }
        removed = set(self.cards) - set(visible)
        for address in removed:
            card = self.cards.pop(address)
            self.grid.removeWidget(card)
            card.deleteLater()
        if removed:
            self._relayout(force=True)
        for address, stats in visible.items():
            card = self._ensure_card(address, stats.name)
            card.update_stats(stats)
            if address == HUB_ADDRESS:
                card.update_clock_diagnostics(diagnostics)
