"""Small widgets and helpers shared by the Simple and Developer views."""

from __future__ import annotations

import math
from pathlib import Path

from PySide6.QtCore import QEvent, QObject, Qt, Signal
from PySide6.QtWidgets import (
    QAbstractSlider,
    QAbstractSpinBox,
    QComboBox,
    QFrame,
    QLabel,
    QScrollArea,
    QSizePolicy,
    QWidget,
)

from ..health import Level
from .style import LEVEL_COLOURS


class UiBridge(QObject):
    """Moves controller callbacks (BLE thread) onto the Qt GUI thread."""

    event = Signal(str, object)


class StatLabel(QLabel):
    def __init__(self, text: str = "-") -> None:
        super().__init__(text)
        self.setObjectName("stat")
        self.setTextInteractionFlags(Qt.TextSelectableByMouse)
        # Live counters must not change the minimum width of their parent
        # layout whenever another digit is added.
        self.setMinimumWidth(0)
        self.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)


def signed_us(value: float) -> str:
    return "-" if not math.isfinite(value) else f"{value:+.3f} µs"


def us(value: float) -> str:
    return "-" if not math.isfinite(value) else f"{value:.3f} µs"


def repolish(widget: QWidget) -> None:
    """Re-apply the stylesheet after a dynamic property changed."""
    widget.style().unpolish(widget)
    widget.style().polish(widget)


def set_pill(label: QLabel, online: bool, text: str) -> None:
    label.setText(text)
    label.setProperty("online", online)
    repolish(label)


def set_level_style(label: QLabel, level: Level, extra: str = "") -> None:
    background, foreground = LEVEL_COLOURS[level]
    label.setStyleSheet(
        f"background: {background}; color: {foreground}; {extra}")


def title_label(text: str, object_name: str = "nodeTitle") -> QLabel:
    label = QLabel(text)
    label.setObjectName(object_name)
    return label


def vertical_scroll(widget: QWidget) -> QScrollArea:
    """Wrap a page so it scrolls vertically and never horizontally."""
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setFrameShape(QFrame.NoFrame)
    scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAsNeeded)
    scroll.verticalScrollBar().setSingleStep(24)
    scroll.setWidget(widget)
    return scroll


class WheelGuard(QObject):
    """Stop the mouse wheel changing controls the pointer merely passes over.

    Pages scroll, so wheeling down would otherwise nudge every slider, spin
    box and combo box under the pointer -- and those send commands to the
    boards (RGB, audio, valve, TTL).  A control only takes the wheel once it
    has been clicked into; until then the wheel scrolls the page.
    """

    GUARDED = (QAbstractSlider, QAbstractSpinBox, QComboBox)

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802
        if (
            event.type() == QEvent.Wheel
            and isinstance(watched, self.GUARDED)
            and not watched.hasFocus()
        ):
            event.ignore()  # propagate up to the enclosing scroll area
            return True
        return False


def recording_directory_path(directory: Path, name: str) -> Path:
    """Return a path in ``directory`` for ``name`` that does not exist yet."""
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
