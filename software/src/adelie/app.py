"""Adelie GUI entry point.

    adelie                 open in the mode used last time (Simple by default)
    adelie --simple        open in Simple mode
    adelie --dev           open in Developer mode
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys

from PySide6.QtWidgets import QApplication

from .gui.common import WheelGuard
from .gui.main_window import MainWindow
from .gui.session_bar import DEVELOPER, SIMPLE
from .gui.style import STYLE


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Adelie control application")
    parser.add_argument("--config", type=Path, default=Path("config/boards.json"))
    parser.add_argument("--recordings", type=Path, default=Path("recordings"))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--simple", dest="mode", action="store_const", const=SIMPLE,
                      help="start in Simple mode")
    mode.add_argument("--dev", dest="mode", action="store_const", const=DEVELOPER,
                      help="start in Developer mode")
    return parser.parse_args(argv)


def main() -> None:
    args = parse_args()
    application = QApplication(sys.argv)
    application.setStyleSheet(STYLE)
    wheel_guard = WheelGuard(application)
    application.installEventFilter(wheel_guard)
    window = MainWindow(args.config, args.recordings, args.mode)
    window.show()
    sys.exit(application.exec())


if __name__ == "__main__":
    main()
