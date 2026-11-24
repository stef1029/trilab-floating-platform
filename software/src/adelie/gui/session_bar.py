"""Top bar shared by both modes: connection, mode switch and recording."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from PySide6.QtCore import Signal
from PySide6.QtWidgets import (
    QButtonGroup,
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
)

from ..controller import AdelieController
from .common import recording_directory_path, set_pill

SIMPLE, DEVELOPER = 0, 1


def _new_log_name() -> str:
    return datetime.now().strftime("run_%Y%m%d_%H%M%S.log")


class SessionBar(QFrame):
    mode_changed = Signal(int)

    def __init__(self, controller: AdelieController, recording_directory: Path) -> None:
        super().__init__()
        self.controller = controller
        self.recording_directory = recording_directory

        title = QLabel("Adelie")
        title.setObjectName("nodeTitle")
        self.connection = QLabel("Disconnected")
        self.connection.setObjectName("statePill")
        self.connection.setProperty("online", False)
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(controller.connect)
        self.disconnect_button = QPushButton("Disconnect")
        self.disconnect_button.setObjectName("secondaryButton")
        self.disconnect_button.clicked.connect(controller.disconnect)
        self.disconnect_button.setEnabled(False)

        self.mode_group = QButtonGroup(self)
        self.mode_group.setExclusive(True)
        mode_row = QHBoxLayout()
        mode_row.setSpacing(0)
        for index, text in ((SIMPLE, "Simple"), (DEVELOPER, "Developer")):
            button = QPushButton(text)
            button.setObjectName("modeButton")
            button.setCheckable(True)
            self.mode_group.addButton(button, index)
            mode_row.addWidget(button)
        self.mode_group.idClicked.connect(self.mode_changed)

        top = QHBoxLayout()
        top.addWidget(title)
        top.addSpacing(8)
        top.addWidget(self.connection)
        top.addStretch()
        top.addLayout(mode_row)
        top.addSpacing(16)
        top.addWidget(self.connect_button)
        top.addWidget(self.disconnect_button)

        self.log_path = QLineEdit(str(self._fresh_log_path()))
        browse = QPushButton("Choose…")
        browse.setObjectName("secondaryButton")
        browse.clicked.connect(self._choose_log)
        self.record_button = QPushButton("●  Start recording")
        self.record_button.setObjectName("recordButton")
        self.record_button.clicked.connect(self._start_recording)
        self.record_button.setEnabled(False)
        self.stop_button = QPushButton("■  Stop recording")
        self.stop_button.clicked.connect(controller.stop_recording)
        self.stop_button.setEnabled(False)

        record = QHBoxLayout()
        record.addWidget(QLabel("Log file"))
        record.addWidget(self.log_path, 1)
        record.addWidget(browse)
        record.addWidget(self.record_button)
        record.addWidget(self.stop_button)

        layout = QVBoxLayout(self)
        layout.addLayout(top)
        layout.addLayout(record)

    def set_mode(self, mode: int) -> None:
        self.mode_group.button(mode).setChecked(True)

    def _fresh_log_path(self) -> Path:
        return recording_directory_path(self.recording_directory, _new_log_name())

    def _choose_log(self) -> None:
        selected, _ = QFileDialog.getSaveFileName(
            self, "Choose run log", self.log_path.text(), "Node log (*.log)"
        )
        if selected:
            self.log_path.setText(
                selected if selected.endswith(".log") else selected + ".log")

    def _start_recording(self) -> None:
        try:
            self.controller.start_recording(Path(self.log_path.text()))
        except Exception as error:
            QMessageBox.critical(self, "Cannot start recording", str(error))

    def handle_event(self, kind: str, value: object) -> None:
        if kind == "connection":
            state = value["state"]
            connected = state == "connected"
            detail = value["detail"]
            set_pill(
                self.connection,
                connected,
                "Connected" if connected else f"{state.capitalize()}"
                + (f": {detail}" if detail and not connected else ""),
            )
            self.connection.setToolTip(str(detail))
            self.connect_button.setEnabled(not connected)
            self.disconnect_button.setEnabled(connected)
        elif kind == "recording":
            active = bool(value["active"])
            self.stop_button.setEnabled(active)
            self.log_path.setEnabled(not active)
            if not active:
                self.log_path.setText(str(self._fresh_log_path()))

    def refresh(self) -> None:
        if not self.controller.recorder.active:
            self.record_button.setEnabled(self.controller.ready_to_record)
        else:
            self.record_button.setEnabled(False)
