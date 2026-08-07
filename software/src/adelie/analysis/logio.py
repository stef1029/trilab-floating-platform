from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Iterator

from adelie.constants import CommandField, Field


def read_log(path: Path) -> Iterator[dict[str, Any]]:
    header_seen = False
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as error:
                raise ValueError(f"{path}:{line_number}: malformed JSON: {error}") from error
            if not header_seen:
                if (
                    item.get("schema") != "fairy_log"
                    or item.get("version") != 1
                    or item.get("kind") != "header"
                ):
                    raise ValueError(f"{path}:{line_number}: unsupported log header")
                header_seen = True
            yield item
    if not header_seen:
        raise ValueError(f"{path} is empty")


def fields_by_name(item: dict[str, Any]) -> dict[str, Any]:
    return _fields_by_enum(item, Field)


def command_fields_by_name(item: dict[str, Any]) -> dict[str, Any]:
    return _fields_by_enum(item, CommandField)


def _fields_by_enum(item: dict[str, Any], field_enum: Any) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for field in item.get("fields", []):
        try:
            name = field_enum(int(field["tag"])).name.lower()
        except ValueError:
            name = f"field_0x{int(field['tag']):04x}"
        value = field.get("value")
        if isinstance(value, dict) and set(value) == {"base64"}:
            value = value["base64"]
        result[name] = value
    return result
