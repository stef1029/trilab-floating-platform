from __future__ import annotations

import json

from adelie.config import RigConfig
from adelie.legacy_names import modernise_keys


def test_log_keys_are_modernised():
    old = {"kind": "marker", "korora_ble_dropped": 3, "galapagos_ttl_generated": 1,
           "protocol": {"transport": 1, "fairy": 3, "adelie": 2, "magellan": 1},
           "assignments": [{"uuid": "ab", "fairy_number": 2}]}
    new = modernise_keys(old)
    assert new["hub_ble_dropped"] == 3 and new["ttl_generator_ttl_generated"] == 1
    assert new["protocol"] == {"transport": 1, "record": 3, "command": 2, "discovery": 1}
    assert new["assignments"] == [{"uuid": "ab", "node_number": 2}]


def test_old_board_config_still_loads(tmp_path):
    path = tmp_path / "boards.json"
    path.write_text(json.dumps({"version": 2, "assignments": [
        {"uuid": "470037000151353530333937", "fairy_number": 1, "label": ""}]}))
    config = RigConfig.load(path)
    assert config.assignments[0].node_number == 1
