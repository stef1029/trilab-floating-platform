from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from pathlib import Path
from typing import Iterable

from .constants import DEFAULT_MAX_FAIRIES


def normalize_uuid(value: str) -> str:
    compact = value.lower().replace("-", "").replace(":", "").strip()
    if len(compact) != 24 or any(character not in "0123456789abcdef" for character in compact):
        raise ValueError("a Fairy UUID must contain exactly 12 bytes")
    return compact


@dataclass(frozen=True, slots=True)
class BoardAssignment:
    uuid: str
    fairy_number: int
    label: str = ""

    def normalized(self) -> "BoardAssignment":
        if not 0 <= self.fairy_number < DEFAULT_MAX_FAIRIES:
            raise ValueError(
                f"Fairy index must be between 0 and {DEFAULT_MAX_FAIRIES - 1}"
            )
        return BoardAssignment(
            uuid=normalize_uuid(self.uuid),
            fairy_number=self.fairy_number,
            label=self.label.strip(),
        )


@dataclass(slots=True)
class RigConfig:
    assignments: list[BoardAssignment]
    version: int = 2

    @classmethod
    def load(cls, path: Path) -> "RigConfig":
        if not path.exists():
            return cls(assignments=[])
        parsed = json.loads(path.read_text(encoding="utf-8"))
        version = parsed.get("version")
        if version not in (1, 2):
            raise ValueError("unsupported rig configuration version")
        raw_assignments = parsed.get("assignments", [])
        if version == 1:
            migrated = []
            for item in raw_assignments:
                old_number = int(item["fairy_number"])
                if not 1 <= old_number <= DEFAULT_MAX_FAIRIES:
                    raise ValueError("invalid Fairy number in version 1 configuration")
                migrated.append(
                    {
                        **item,
                        "fairy_number": old_number - 1,
                    }
                )
            raw_assignments = migrated
        assignments = [
            BoardAssignment(**item).normalized()
            for item in raw_assignments
        ]
        result = cls(assignments=assignments, version=2)
        result.validate()
        return result

    def save(self, path: Path) -> None:
        self.validate()
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(
            json.dumps(
                {
                    "version": self.version,
                    "assignments": [asdict(item.normalized()) for item in self.assignments],
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)

    def validate(self) -> None:
        normalized = [item.normalized() for item in self.assignments]
        uuids = [item.uuid for item in normalized]
        numbers = [item.fairy_number for item in normalized]
        if len(set(uuids)) != len(uuids):
            raise ValueError("duplicate UUID in rig configuration")
        if len(set(numbers)) != len(numbers):
            raise ValueError("duplicate Fairy number in rig configuration")

    def exact_match(self, discovered_uuids: Iterable[str]) -> bool:
        configured = {normalize_uuid(item.uuid) for item in self.assignments}
        discovered = {normalize_uuid(value) for value in discovered_uuids}
        return bool(configured) and configured == discovered

    def assignment_for(self, uuid: str) -> BoardAssignment | None:
        wanted = normalize_uuid(uuid)
        return next((item for item in self.assignments if item.uuid == wanted), None)
