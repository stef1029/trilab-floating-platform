"""System health summary shared by the Simple and Developer views.

This module has no Qt dependency.  It turns the controller's live state into a
short list of plain-language health items, each rated OK / WARN / ERROR, plus a
single "ready to record" verdict with the reason when it is not ready.

The controller is duck-typed (see ``HealthSource``) so tests can pass a plain
namespace instead of a live BLE controller.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from enum import IntEnum
import time
from typing import Any, Mapping, Protocol, Sequence

from .constants import NODE_ADDRESS_BASE, TTL_GENERATOR_ADDRESS, HUB_ADDRESS
from .models import LocalSensorState, NodeStats

# A node that has sent nothing for this long is shown as offline.  node
# health/sync records arrive at roughly 1 Hz, so this allows a few misses.
NODE_STALE_NS = 5_000_000_000
# How long the beam-break indicator stays lit after a light-gate event.
BEAM_HOLD_NS = 1_000_000_000
# Hub streams many records a second; silence this long means trouble.
LINK_STALE_MS = 2_000.0
# Window over which error counters are turned into rates.
RATE_WINDOW_NS = 5_000_000_000
# Error-rate thresholds (errors per second, all comms counters summed).
COMMS_WARN_PER_S = 0.2
COMMS_ERROR_PER_S = 5.0
BATTERY_WARN_PERCENT = 40.0
BATTERY_ERROR_PERCENT = 20.0
# Used when the PMIC reports voltage but no state of charge.  The nPM1300
# charges a single Li-ion / LiPo cell: ~3.7 V is roughly a third left, and
# below ~3.5 V the cell is nearly empty.
BATTERY_WARN_MILLIVOLTS = 3_700
BATTERY_ERROR_MILLIVOLTS = 3_500


class Level(IntEnum):
    UNKNOWN = 0
    OK = 1
    WARN = 2
    ERROR = 3


class NodeState(IntEnum):
    OFFLINE = 0
    ONLINE = 1  # talking, clock model not yet locked
    READY = 2  # talking and clock-synchronised


def node_state(stats: NodeStats | None, now_ns: int | None = None) -> NodeState:
    """Classify a node by how recently it spoke, not by sticky flags."""
    if stats is None or not stats.connected or stats.last_record_ns == 0:
        return NodeState.OFFLINE
    now = time.perf_counter_ns() if now_ns is None else now_ns
    if now - stats.last_record_ns > NODE_STALE_NS:
        return NodeState.OFFLINE
    return NodeState.READY if stats.synchronized else NodeState.ONLINE


def beam_active(stats: NodeStats | None, now_ns: int | None = None) -> bool:
    if stats is None or stats.last_light_gate_ns == 0:
        return False
    now = time.perf_counter_ns() if now_ns is None else now_ns
    return 0 <= now - stats.last_light_gate_ns < BEAM_HOLD_NS


@dataclass(frozen=True, slots=True)
class BoardInfo:
    """One configured node board as the Simple view needs to see it."""

    node_number: int
    address: int
    name: str


@dataclass(frozen=True, slots=True)
class HealthItem:
    key: str
    title: str
    level: Level
    detail: str


@dataclass(frozen=True, slots=True)
class SystemHealth:
    items: tuple[HealthItem, ...]
    ready_to_record: bool
    ready_reason: str
    comms_errors_per_s: float = 0.0

    def item(self, key: str) -> HealthItem:
        return next(value for value in self.items if value.key == key)

    @property
    def worst(self) -> Level:
        return max((value.level for value in self.items), default=Level.UNKNOWN)


class HealthSource(Protocol):
    connected: bool
    # Present node boards by node number.
    inventory: Mapping[int, Any]
    nodes: Mapping[int, NodeStats]
    local_sensors: LocalSensorState
    config: Any

    @property
    def ready_to_record(self) -> bool: ...


def configured_boards(config: Any) -> list[BoardInfo]:
    return [
        BoardInfo(
            node_number=assignment.node_number,
            address=NODE_ADDRESS_BASE + assignment.node_number,
            name=assignment.name,
        )
        for assignment in sorted(
            config.assignments, key=lambda item: item.node_number
        )
    ]


@dataclass
class _RateTracker:
    """Turns monotonically increasing counters into a recent per-second rate."""

    window_ns: int = RATE_WINDOW_NS
    _samples: deque[tuple[int, int]] = field(default_factory=deque)

    def update(self, now_ns: int, total: int) -> float:
        if self._samples and total < self._samples[-1][1]:
            self._samples.clear()  # counters reset (reconnect / reboot)
        self._samples.append((now_ns, total))
        cutoff = now_ns - self.window_ns
        while len(self._samples) > 2 and self._samples[1][0] <= cutoff:
            self._samples.popleft()
        if len(self._samples) < 2:
            return 0.0
        start_ns, start_total = self._samples[0]
        elapsed = (now_ns - start_ns) / 1e9
        return max(0, total - start_total) / elapsed if elapsed > 0 else 0.0


def comms_error_total(diagnostics: Mapping[str, Any]) -> int:
    keys = (
        "rx_decode_errors",
        "rx_reassembly_errors",
        "rx_address_errors",
        "tx_write_errors",
        "hub_rs485_errors",
        "hub_ble_dropped",
    )
    return sum(int(diagnostics.get(key, 0) or 0) for key in keys)


class HealthMonitor:
    """Call ``assess`` once per GUI refresh; it keeps the rate history."""

    def __init__(self) -> None:
        self._comms = _RateTracker()

    def assess(
        self,
        source: HealthSource,
        diagnostics: Mapping[str, Any],
        now_ns: int | None = None,
    ) -> SystemHealth:
        now = time.perf_counter_ns() if now_ns is None else now_ns
        comms_rate = self._comms.update(now, comms_error_total(diagnostics))
        boards = configured_boards(source.config)

        items = (
            _hub_item(source, diagnostics),
            _battery_item(source),
            _boards_item(source, boards, now),
            _sync_box_item(source, now),
            _comms_item(source, comms_rate),
        )
        ready = bool(source.ready_to_record)
        return SystemHealth(
            items=items,
            ready_to_record=ready,
            ready_reason="Ready to record" if ready else _not_ready_reason(
                source, boards, now),
            comms_errors_per_s=comms_rate,
        )


def _hub_item(source: HealthSource, diagnostics: Mapping[str, Any]) -> HealthItem:
    if not source.connected:
        return HealthItem("hub", "Hub", Level.ERROR, "Not connected")
    age = diagnostics.get("last_rx_age_ms")
    if age is None:
        return HealthItem("hub", "Hub", Level.WARN, "Connected, waiting for data")
    if float(age) > LINK_STALE_MS:
        return HealthItem(
            "hub", "Hub", Level.WARN, f"No data for {float(age) / 1000:.0f} s"
        )
    return HealthItem("hub", "Hub", Level.OK, "Connected")


def _battery_item(source: HealthSource) -> HealthItem:
    sensors = source.local_sensors
    if not source.connected or not sensors.pmic_present:
        return HealthItem("battery", "Battery", Level.UNKNOWN, "-")
    if sensors.charger_error:
        return HealthItem(
            "battery", "Battery", Level.ERROR, f"Charger: {sensors.charger_error}"
        )
    percent = (
        None if sensors.battery_soc_per_mille is None
        else sensors.battery_soc_per_mille / 10.0
    )
    millivolts = sensors.battery_millivolts
    if percent is not None:
        detail = f"{percent:.0f}%"
        if percent < BATTERY_ERROR_PERCENT:
            level = Level.ERROR
        elif percent < BATTERY_WARN_PERCENT:
            level = Level.WARN
        else:
            level = Level.OK
    elif millivolts is not None:
        detail = f"{millivolts / 1000:.2f} V"
        if millivolts < BATTERY_ERROR_MILLIVOLTS:
            level = Level.ERROR
        elif millivolts < BATTERY_WARN_MILLIVOLTS:
            level = Level.WARN
        else:
            level = Level.OK
    else:
        return HealthItem("battery", "Battery", Level.UNKNOWN, "No reading")
    charging = sensors.charge_status.lower()
    if "charg" in charging and "not" not in charging and "complete" not in charging:
        detail += " · charging"
    return HealthItem("battery", "Battery", level, detail)


def _unexpected_boards(source: HealthSource, boards: Sequence[BoardInfo]) -> list[int]:
    """Node numbers the hub sees that this rig's configuration does not list."""
    configured = {board.node_number for board in boards}
    return sorted(number for number in source.inventory if number not in configured)


def _boards_item(
    source: HealthSource, boards: Sequence[BoardInfo], now_ns: int
) -> HealthItem:
    if not boards:
        return HealthItem(
            "boards", "Ports", Level.WARN, "No ports configured (Developer › Board setup)"
        )
    if not source.connected:
        return HealthItem("boards", "Ports", Level.UNKNOWN, f"{len(boards)} configured")
    unexpected = _unexpected_boards(source, boards)
    extra = (
        " · unconfigured node " + ", ".join(str(number) for number in unexpected)
        if unexpected else ""
    )
    missing = [
        board.name
        for board in boards
        if node_state(source.nodes.get(board.address), now_ns) is not NodeState.READY
    ]
    ready = len(boards) - len(missing)
    if not missing:
        return HealthItem(
            "boards", "Ports", Level.WARN if unexpected else Level.OK,
            f"All {ready} ready{extra}",
        )
    shown = ", ".join(missing[:3]) + (" …" if len(missing) > 3 else "")
    return HealthItem(
        "boards", "Ports", Level.WARN,
        f"{ready} of {len(boards)} ready · waiting for {shown}{extra}",
    )


def _sync_box_item(source: HealthSource, now_ns: int) -> HealthItem:
    if not source.connected:
        return HealthItem("sync", "TTL generator", Level.UNKNOWN, "-")
    state = node_state(source.nodes.get(TTL_GENERATOR_ADDRESS), now_ns)
    if state is NodeState.READY:
        return HealthItem("sync", "TTL generator", Level.OK, "Synchronised")
    if state is NodeState.ONLINE:
        return HealthItem("sync", "TTL generator", Level.WARN, "Synchronising…")
    return HealthItem("sync", "TTL generator", Level.ERROR, "Offline")


def _comms_item(source: HealthSource, rate: float) -> HealthItem:
    if not source.connected:
        return HealthItem("comms", "Comms", Level.UNKNOWN, "-")
    if rate >= COMMS_ERROR_PER_S:
        level = Level.ERROR
    elif rate >= COMMS_WARN_PER_S:
        level = Level.WARN
    else:
        return HealthItem("comms", "Comms", Level.OK, "No errors")
    return HealthItem("comms", "Comms", level, f"{rate:.1f} errors/s")


def _not_ready_reason(
    source: HealthSource, boards: Sequence[BoardInfo], now_ns: int
) -> str:
    if not source.connected:
        return "Connect to the hub to start"
    if not boards:
        return "No ports configured: use Developer › Board setup"
    ttl_generator = node_state(source.nodes.get(TTL_GENERATOR_ADDRESS), now_ns)
    if ttl_generator is NodeState.OFFLINE:
        return "Sync box is offline"
    waiting = [
        board.name
        for board in boards
        if node_state(source.nodes.get(board.address), now_ns) is not NodeState.READY
    ]
    if waiting:
        return f"Waiting for {', '.join(waiting)}"
    if ttl_generator is not NodeState.READY:
        return "Waiting for the sync box clock"
    hub = source.nodes.get(HUB_ADDRESS)
    if hub is None or not hub.connected:
        return "Waiting for the hub"
    return "Waiting for clock synchronisation"
