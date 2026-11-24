"""Application stylesheet and the colours that code also needs (plots, pills)."""

from __future__ import annotations

from ..health import Level

BACKGROUND = "#16131f"
PANEL = "#211c2d"
TEXT = "#eeeaf8"
MUTED = "#9b91ad"
ACCENT = "#7857d6"

LEVEL_COLOURS = {
    Level.UNKNOWN: ("#3a3345", "#c9c2d6"),
    Level.OK: ("#245a48", "#baf4dc"),
    Level.WARN: ("#6b5321", "#ffe3a3"),
    Level.ERROR: ("#6e2836", "#ffc9d3"),
}

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
QFrame#plain, QScrollArea, QStackedWidget { border: 0; background: #16131f; }
/* QLabel is a QFrame subclass: undo the panel look for plain text. */
QLabel { border: 0; background: transparent; }
QLabel#stat {
    background: #1b1726;
    border: 1px solid #2f2840;
    border-radius: 5px;
    padding: 1px 5px;
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
QPushButton#dangerButton:hover { background: #c95f80; }
QPushButton#secondaryButton { background: #3a3150; }
QPushButton#secondaryButton:hover { background: #4b4065; }
QPushButton#modeButton {
    background: #2a2338;
    border-radius: 0;
    padding: 6px 14px;
}
QPushButton#modeButton:checked { background: #7857d6; }
QPushButton#recordButton { background: #b34b6c; }
QPushButton#recordButton:hover { background: #c95f80; }
QPushButton#dangerButton:disabled, QPushButton#recordButton:disabled,
QPushButton#secondaryButton:disabled {
    background: #3a3345;
    color: #8e879b;
}
QPushButton#swatch {
    min-width: 30px; max-width: 30px;
    min-height: 30px; max-height: 30px;
    padding: 0;
    border-radius: 15px;
    border: 2px solid #3a3150;
}
QPushButton#swatch:hover { border: 2px solid #eeeaf8; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QTableWidget {
    background: #191522;
    border: 1px solid #4b4065;
    border-radius: 6px;
    padding: 5px;
}
QTabWidget::pane { border: 0; }
QTabBar { border: 0; background: transparent; }
QTabBar::tab {
    background: #211c2d;
    padding: 8px 16px;
    margin-right: 2px;
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
}
QTabBar::tab:selected { background: #7857d6; }
QLabel#nodeTitle { font-size: 19px; font-weight: 700; color: #cebfff; }
QLabel#sectionTitle { font-size: 15px; font-weight: 700; color: #cebfff; }
QLabel#muted { color: #9b91ad; }
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
QLabel#readyBanner {
    border-radius: 10px;
    padding: 10px 14px;
    font-size: 15px;
    font-weight: 700;
}
"""
