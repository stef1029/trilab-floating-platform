"""Read sync-test data recorded before the codename rename.

Serial logs and host CSVs recorded before the rename say korora / fairy /
galapagos (e.g. ``SYNC,fairy,...`` or a ``korora_rx_ticks`` column).  The
parsers now use hub / node / ttl_generator, so raw input is passed through
``modernise`` as it is read.  Recorded data itself is never rewritten.
"""

from __future__ import annotations

import re

_RULES = tuple(
    (re.compile(old), new)
    for old, new in (
        ("korora", "hub"), ("KORORA", "HUB"), ("Korora", "Hub"),
        ("galapagos", "ttl_generator"), ("GALAPAGOS", "TTL_GENERATOR"),
        ("Galapagos", "TTL generator"),
        ("fairy", "node"), ("FAIRY", "NODE"), ("Fairy", "Node"),
    )
)


def modernise(text: str) -> str:
    """Rename legacy codenames inside one raw log line or CSV field."""
    for pattern, replacement in _RULES:
        text = pattern.sub(replacement, text)
    return text
