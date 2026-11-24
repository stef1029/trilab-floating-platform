"""Read files written before the codename rename (Korora, Fairy, Galapagos...).

Logs and board configs recorded before the rename use the old names in their
JSON keys.  Rather than rewrite recorded data, readers pass each parsed object
through ``modernise_keys`` so the rest of the code only sees the new names.

    korora    -> hub              fairy (board) -> node
    galapagos -> ttl_generator    protocol keys: fairy -> record,
    magellan  -> discovery                       adelie -> command
"""

from __future__ import annotations

import re
from typing import Any

LEGACY_LOG_SCHEMAS = frozenset({"fairy_log"})

# Whole-key renames: the protocol-version table in the log header.
_EXACT = {"fairy": "record", "adelie": "command", "magellan": "discovery"}
_PARTS = (
    (re.compile(r"korora"), "hub"),
    (re.compile(r"galapagos"), "ttl_generator"),
    (re.compile(r"magellan"), "discovery"),
    (re.compile(r"fairies"), "nodes"),
    (re.compile(r"fairy"), "node"),
)


def modern_key(key: str) -> str:
    if key in _EXACT:
        return _EXACT[key]
    for pattern, replacement in _PARTS:
        key = pattern.sub(replacement, key)
    return key


def modernise_keys(value: Any) -> Any:
    """Return ``value`` with every dict key renamed, recursively."""
    if isinstance(value, dict):
        return {modern_key(key) if isinstance(key, str) else key: modernise_keys(item)
                for key, item in value.items()}
    if isinstance(value, list):
        return [modernise_keys(item) for item in value]
    return value
