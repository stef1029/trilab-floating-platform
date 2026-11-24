"""Board configuration and node inventory with flashed node numbers.

Node boards are numbered when they are flashed and node N always answers at
NODE_ADDRESS_BASE + N, so Adelie only tracks which numbers are present.
"""

from __future__ import annotations

import json
from types import SimpleNamespace

import pytest

from adelie.config import BoardAssignment, RigConfig
from adelie.constants import DEFAULT_MAX_NODES, NODE_ADDRESS_BASE, Field, Opcode
from adelie.controller import AdelieController

UUID_3 = bytes.fromhex("470037000151353530333937")


# ---- configuration ----------------------------------------------------

def test_missing_config_uses_every_node_number(tmp_path):
    config = RigConfig.load(tmp_path / "boards.json")
    assert config.node_numbers == list(range(DEFAULT_MAX_NODES))


def test_version_2_config_drops_uuids_and_keeps_numbers(tmp_path):
    path = tmp_path / "boards.json"
    path.write_text(json.dumps({"version": 2, "assignments": [
        {"uuid": "470037000151353530333937", "node_number": 3, "label": "north"},
        {"uuid": "48004f000151353530333937", "node_number": 0, "label": ""},
    ]}))
    config = RigConfig.load(path)
    assert config.node_numbers == [0, 3]
    assert config.assignment_for(3).label == "north"


def test_version_1_numbers_from_one(tmp_path):
    path = tmp_path / "boards.json"
    path.write_text(json.dumps({"version": 1, "assignments": [
        {"uuid": "470037000151353530333937", "node_number": 1, "label": ""}]}))
    assert RigConfig.load(path).node_numbers == [0]


def test_save_writes_version_3_without_uuids(tmp_path):
    path = tmp_path / "boards.json"
    RigConfig(assignments=[
        BoardAssignment(node_number=2, label=" south "),
        BoardAssignment(node_number=0),
    ]).save(path)
    saved = json.loads(path.read_text())
    assert saved == {"version": 3, "assignments": [
        {"node_number": 0, "label": ""},
        {"node_number": 2, "label": "south"},
    ]}
    assert RigConfig.load(path).node_numbers == [0, 2]


def test_duplicate_and_out_of_range_numbers_are_rejected():
    with pytest.raises(ValueError):
        RigConfig(assignments=[BoardAssignment(1), BoardAssignment(1)]).validate()
    with pytest.raises(ValueError):
        BoardAssignment(DEFAULT_MAX_NODES).normalized()


# ---- controller inventory ---------------------------------------------

@pytest.fixture
def controller(tmp_path):
    path = tmp_path / "boards.json"
    RigConfig(assignments=[BoardAssignment(0), BoardAssignment(3, "north")]).save(path)
    events: list[tuple[str, object]] = []
    result = AdelieController(path, lambda kind, value: events.append((kind, value)))
    result.events = events
    result.sent = []
    result.stops = []
    result.connected = True
    # Capture commands and stop requests instead of using BLE and timers.
    result.command = lambda opcode, **kwargs: result.sent.append(
        (opcode, kwargs.get("destination"))) or 1
    result._request_stop = result.stops.append
    return result


def _inventory_record(node_number: int, present: bool, uuid: bytes = b"\0" * 12):
    return SimpleNamespace(field_map={
        int(Field.UUID): uuid,
        int(Field.LINK_ADDRESS): NODE_ADDRESS_BASE + node_number,
        int(Field.LOGICAL_SLOT): node_number,
        int(Field.VALUE): 0x3F,
        int(Field.STATE): present,
    })


def _last_inventory_event(controller):
    return next(value for kind, value in reversed(controller.events)
                if kind == "inventory")


def test_configured_nodes_are_named_from_labels(controller):
    assert controller.nodes[NODE_ADDRESS_BASE + 3].name == "north"
    assert controller.nodes[NODE_ADDRESS_BASE + 0].name == "node0"


def test_node_appearing_is_added_and_gets_valve_settings(controller):
    controller._update_inventory_record(_inventory_record(3, True, UUID_3))
    entry = controller.inventory[3]
    assert entry.address == NODE_ADDRESS_BASE + 3 and entry.uuid == UUID_3.hex()
    assert (Opcode.CONFIGURE_VALVE, NODE_ADDRESS_BASE + 3) in controller.sent
    event = _last_inventory_event(controller)
    assert event["missing"] == [0] and event["unexpected"] == []


def test_zero_uuid_means_not_reported(controller):
    controller._update_inventory_record(_inventory_record(0, True))
    assert controller.inventory[0].uuid is None


def test_unconfigured_node_is_reported_as_unexpected(controller):
    controller._update_inventory_record(_inventory_record(4, True))
    assert _last_inventory_event(controller)["unexpected"] == [4]


def test_required_node_leaving_stops_the_session(controller):
    controller._update_inventory_record(_inventory_record(3, True))
    controller.session_id = 7
    controller._update_inventory_record(_inventory_record(3, False))
    assert 3 not in controller.inventory
    assert controller.stops == ["Node disconnected"]


def test_required_node_restarting_stops_the_session(controller):
    controller._update_inventory_record(_inventory_record(3, True))
    controller.session_id = 7
    controller._update_inventory_record(_inventory_record(3, True, UUID_3))
    assert controller.stops == ["Node restarted"]


def test_unconfigured_node_changes_do_not_stop_the_session(controller):
    controller._update_inventory_record(_inventory_record(4, True))
    controller.session_id = 7
    controller._update_inventory_record(_inventory_record(4, False))
    assert controller.stops == []


def test_get_inventory_response_replaces_the_set(controller):
    controller._update_inventory_record(_inventory_record(4, True))
    raw = UUID_3 + bytes((NODE_ADDRESS_BASE + 3, 3)) + (0x3F).to_bytes(4, "little")
    response = SimpleNamespace(fields=(
        SimpleNamespace(tag=0x1001, value=raw),  # CommandField.UUID_LIST
    ))
    controller._decode_inventory_response(response)
    assert set(controller.inventory) == {3}
    assert controller.inventory[3].uuid == UUID_3.hex()
