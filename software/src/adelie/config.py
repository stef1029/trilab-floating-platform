"""Rig board configuration (config/boards.json).

Node boards are numbered when they are flashed
(firmware/reward_port/scripts/flash.py --id N), and node N always uses RS485
address 0x10 + N. The configuration therefore only says which node numbers
this rig uses and what each port is called. Every listed node is required to
start a recording.

    {"version": 3,
     "assignments": [{"node_number": 0, "label": "north"}, ...]}

Versions 1 and 2 also mapped each board's UUID to its number. Those UUIDs
are ignored on load; numbers and labels are kept.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from pathlib import Path

from .constants import DEFAULT_MAX_NODES
from .legacy_names import modernise_keys

CONFIG_VERSION = 3


@dataclass(frozen=True, slots=True)
class BoardAssignment:
    """One port on the rig: a node number and an optional label."""

    node_number: int
    label: str = ""

    def normalized(self) -> "BoardAssignment":
        if not 0 <= self.node_number < DEFAULT_MAX_NODES:
            raise ValueError(
                f"Node number must be between 0 and {DEFAULT_MAX_NODES - 1}"
            )
        return BoardAssignment(
            node_number=self.node_number,
            label=self.label.strip(),
        )

    @property
    def name(self) -> str:
        return self.label or f"node{self.node_number}"


def default_assignments() -> list[BoardAssignment]:
    """Every node number, unlabelled: used when no boards.json exists."""
    return [BoardAssignment(node_number=number)
            for number in range(DEFAULT_MAX_NODES)]


@dataclass(slots=True)
class RigConfig:
    assignments: list[BoardAssignment]
    version: int = CONFIG_VERSION

    @classmethod
    def load(cls, path: Path) -> "RigConfig":
        if not path.exists():
            return cls(assignments=default_assignments())
        parsed = json.loads(path.read_text(encoding="utf-8"))
        version = parsed.get("version")
        if version not in (1, 2, CONFIG_VERSION):
            raise ValueError("unsupported rig configuration version")
        raw_assignments = modernise_keys(parsed.get("assignments", []))
        assignments = []
        for item in raw_assignments:
            number = int(item["node_number"])
            if version == 1:
                # Version 1 numbered nodes from 1.
                if not 1 <= number <= DEFAULT_MAX_NODES:
                    raise ValueError(
                        "invalid node number in version 1 configuration")
                number -= 1
            assignments.append(
                BoardAssignment(
                    node_number=number,
                    label=str(item.get("label", "")),
                ).normalized()
            )
        result = cls(assignments=assignments)
        result.validate()
        return result

    def save(self, path: Path) -> None:
        self.validate()
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(
            json.dumps(
                {
                    "version": CONFIG_VERSION,
                    "assignments": [
                        asdict(item.normalized())
                        for item in sorted(
                            self.assignments, key=lambda item: item.node_number
                        )
                    ],
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)

    def validate(self) -> None:
        numbers = [item.normalized().node_number for item in self.assignments]
        if len(set(numbers)) != len(numbers):
            raise ValueError("duplicate node number in rig configuration")

    @property
    def node_numbers(self) -> list[int]:
        return sorted(item.node_number for item in self.assignments)

    def assignment_for(self, node_number: int) -> BoardAssignment | None:
        return next(
            (item for item in self.assignments if item.node_number == node_number),
            None,
        )
