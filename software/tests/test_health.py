from __future__ import annotations

from types import SimpleNamespace

from adelie.config import BoardAssignment, RigConfig
from adelie.constants import NODE_ADDRESS_BASE, TTL_GENERATOR_ADDRESS, HUB_ADDRESS
from adelie.health import (
    BEAM_HOLD_NS,
    NODE_STALE_NS,
    HealthMonitor,
    Level,
    NodeState,
    beam_active,
    node_state,
)
from adelie.models import LocalSensorState, NodeStats

NOW = 1_000_000_000_000
UUIDS = ["470037000151353530333937", "48004f000151353530333937"]


def _node(address: int, *, synced: bool = True, age_ns: int = 0) -> NodeStats:
    stats = NodeStats(address, f"n{address}")
    stats.connected = True
    stats.synchronized = synced
    stats.last_record_ns = NOW - age_ns
    return stats


def _source(**overrides):
    config = RigConfig(assignments=[
        BoardAssignment(uuid=UUIDS[0], node_number=0, label=""),
        BoardAssignment(uuid=UUIDS[1], node_number=1, label="left"),
    ])
    nodes = {
        HUB_ADDRESS: _node(HUB_ADDRESS),
        TTL_GENERATOR_ADDRESS: _node(TTL_GENERATOR_ADDRESS),
        NODE_ADDRESS_BASE + 0: _node(NODE_ADDRESS_BASE + 0),
        NODE_ADDRESS_BASE + 1: _node(NODE_ADDRESS_BASE + 1),
    }
    values = dict(
        connected=True,
        inventory_applied=True,
        inventory={uuid: object() for uuid in UUIDS},
        nodes=nodes,
        local_sensors=LocalSensorState(),
        config=config,
        ready_to_record=True,
    )
    values.update(overrides)
    return SimpleNamespace(**values)


DIAG = {"last_rx_age_ms": 10.0}


def test_node_state_uses_record_age_not_sticky_flags():
    assert node_state(None, NOW) is NodeState.OFFLINE
    assert node_state(_node(1), NOW) is NodeState.READY
    assert node_state(_node(1, synced=False), NOW) is NodeState.ONLINE
    assert node_state(_node(1, age_ns=NODE_STALE_NS + 1), NOW) is NodeState.OFFLINE


def test_beam_active_holds_for_a_moment():
    stats = _node(1)
    assert not beam_active(stats, NOW)
    stats.last_light_gate_ns = NOW - BEAM_HOLD_NS // 2
    assert beam_active(stats, NOW)
    stats.last_light_gate_ns = NOW - BEAM_HOLD_NS - 1
    assert not beam_active(stats, NOW)


def test_all_good():
    health = HealthMonitor().assess(_source(), DIAG, NOW)
    assert health.ready_to_record
    assert health.item("hub").level is Level.OK
    assert health.item("boards").detail == "All 2 ready"
    assert health.item("sync").level is Level.OK


def test_disconnected():
    health = HealthMonitor().assess(
        _source(connected=False, ready_to_record=False), {}, NOW)
    assert health.item("hub").level is Level.ERROR
    assert health.ready_reason == "Connect to the hub to start"


def test_missing_board_is_named():
    source = _source(ready_to_record=False)
    source.nodes[NODE_ADDRESS_BASE + 1].last_record_ns = NOW - NODE_STALE_NS - 1
    health = HealthMonitor().assess(source, DIAG, NOW)
    item = health.item("boards")
    assert item.level is Level.WARN
    assert "1 of 2" in item.detail and "left" in item.detail
    assert health.ready_reason == "Waiting for left"


def test_board_setup_needed_when_uuid_set_differs():
    source = _source(
        inventory_applied=False,
        inventory={UUIDS[0]: object(), "ffffffffffffffffffffffff": object()},
        ready_to_record=False,
    )
    health = HealthMonitor().assess(source, DIAG, NOW)
    assert "Board setup" in health.item("boards").detail
    assert "Board setup" in health.ready_reason


def test_comms_error_rate():
    monitor = HealthMonitor()
    source = _source()
    monitor.assess(source, {**DIAG, "hub_rs485_errors": 0}, NOW)
    health = monitor.assess(
        source, {**DIAG, "hub_rs485_errors": 20}, NOW + 2_000_000_000)
    assert health.comms_errors_per_s == 10.0
    assert health.item("comms").level is Level.ERROR
    # A counter reset (reconnect) must not produce a negative / huge rate.
    health = monitor.assess(
        source, {**DIAG, "hub_rs485_errors": 0}, NOW + 3_000_000_000)
    assert health.item("comms").level is Level.OK


def test_battery_levels():
    sensors = LocalSensorState(pmic_present=True, battery_soc_per_mille=150)
    health = HealthMonitor().assess(_source(local_sensors=sensors), DIAG, NOW)
    assert health.item("battery").level is Level.ERROR
    sensors.battery_soc_per_mille = 800
    sensors.charge_status = "Charging"
    health = HealthMonitor().assess(_source(local_sensors=sensors), DIAG, NOW)
    assert health.item("battery").level is Level.OK
    assert health.item("battery").detail == "80% · charging"


def test_battery_falls_back_to_voltage_without_soc():
    sensors = LocalSensorState(pmic_present=True, battery_millivolts=3_920)
    health = HealthMonitor().assess(_source(local_sensors=sensors), DIAG, NOW)
    assert health.item("battery").level is Level.OK
    assert health.item("battery").detail == "3.92 V"
    sensors.battery_millivolts = 3_400
    health = HealthMonitor().assess(_source(local_sensors=sensors), DIAG, NOW)
    assert health.item("battery").level is Level.ERROR
