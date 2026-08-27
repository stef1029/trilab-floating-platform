from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
import math
import statistics
import time

from .constants import COMMON_TIMER_HZ


@dataclass(slots=True)
class InventoryEntry:
    uuid: str
    address: int
    logical_slot: int | None
    capabilities: int
    last_seen_ns: int = field(default_factory=time.perf_counter_ns)


@dataclass(slots=True)
class NodeStats:
    address: int
    name: str
    connected: bool = False
    synchronized: bool = False
    rms_ns: float = math.nan
    skew_ppb: int = 0
    model_points: int = 0
    record_rate_hz: float = 0.0
    queue_depth: int = 0
    dropped_records: int = 0
    transport_errors: int = 0
    retries: int = 0
    timeout_count: int = 0
    decode_errors: int = 0
    reassembly_errors: int = 0
    transmit_errors: int = 0
    rssi_dbm: int | None = None
    last_record_ns: int = 0
    light_gate_events: int = 0
    last_light_gate_ns: int = 0
    ttl_events: int = 0
    last_ttl_sequence: int = 0
    last_ttl_error_us: float = math.nan
    last_ttl_ns: int = 0
    ttl_capture_count: int = 0
    ttl_capture_drops: int = 0
    _recent_records: deque[int] = field(
        default_factory=lambda: deque(maxlen=256))

    def note_record(self, received_ns: int) -> None:
        self.last_record_ns = received_ns
        self._recent_records.append(received_ns)
        cutoff = received_ns - 5_000_000_000
        while self._recent_records and self._recent_records[0] < cutoff:
            self._recent_records.popleft()
        if len(self._recent_records) >= 2:
            span = (self._recent_records[-1] - self._recent_records[0]) / 1e9
            self.record_rate_hz = (
                len(self._recent_records) - 1) / span if span else 0.0

    def note_light_gate(self, received_ns: int) -> None:
        self.light_gate_events += 1
        self.last_light_gate_ns = received_ns

    def note_ttl(
        self, sequence: int, received_ns: int, error_us: float = math.nan
    ) -> None:
        self.ttl_events += 1
        self.last_ttl_sequence = sequence
        self.last_ttl_error_us = error_us
        self.last_ttl_ns = received_ns


@dataclass(slots=True)
class LocalSensorState:
    i2c_ready: bool = False
    imu_present: bool = False
    magnetometer_present: bool = False
    pmic_present: bool = False
    sample_rate_hz: int = 0
    sample_count: int = 0
    i2c_errors: int = 0
    accel_mg: tuple[int, int, int] = (0, 0, 0)
    gyro_mdps: tuple[int, int, int] = (0, 0, 0)
    mag_milligauss: tuple[int, int, int] = (0, 0, 0)
    raw_chunks_received: int = 0
    raw_samples_received: int = 0
    last_chunk_sequence: int = 0
    last_update_ns: int = 0
    power_source: str = "nRF52840 DK"
    supply_millivolts: int | None = None
    battery_millivolts: int | None = None
    battery_current_ma: int | None = None
    battery_soc_per_mille: int | None = None
    battery_health_per_mille: int | None = None
    charge_status: str = ""
    charger_error: str = ""


@dataclass(slots=True)
class TtlStats:
    session_id: int
    sequence: int
    target_korora_ticks: int | None = None
    generated_target_local_ticks: int | None = None
    generated_local_ticks: int | None = None
    generated_korora_ticks: int | None = None
    captured_korora_ticks: int | None = None
    output_error_us: float = math.nan
    input_error_us: float = math.nan
    loop_delay_us: float = math.nan
    result_error_us: float = math.nan
    latest_record: str = ""
    received_ns: int = 0

    def recompute(self) -> None:
        tick_us = 1_000_000 / COMMON_TIMER_HZ
        if (
            self.target_korora_ticks is not None
            and self.generated_korora_ticks is not None
        ):
            self.output_error_us = (
                self.generated_korora_ticks - self.target_korora_ticks
            ) * tick_us
        if (
            self.target_korora_ticks is not None
            and self.captured_korora_ticks is not None
        ):
            self.input_error_us = (
                self.captured_korora_ticks - self.target_korora_ticks
            ) * tick_us
        if (
            self.generated_korora_ticks is not None
            and self.captured_korora_ticks is not None
        ):
            self.loop_delay_us = (
                self.captured_korora_ticks - self.generated_korora_ticks
            ) * tick_us


@dataclass(frozen=True, slots=True)
class ClockExchange:
    t1_ns: int
    t2_ticks: int
    t3_ticks: int
    t4_ns: int

    @property
    def network_rtt_ns(self) -> float:
        processing_ns = (self.t3_ticks - self.t2_ticks) * 1e9 / COMMON_TIMER_HZ
        return (self.t4_ns - self.t1_ns) - processing_ns

    @property
    def host_midpoint_ns(self) -> float:
        return (self.t1_ns + self.t4_ns) / 2.0

    @property
    def korora_midpoint_ticks(self) -> float:
        return (self.t2_ticks + self.t3_ticks) / 2.0


@dataclass(frozen=True, slots=True)
class HostClockQuality:
    valid: bool
    points: int
    slope_ticks_per_ns: float = math.nan
    intercept_ticks: float = math.nan
    rms_ns: float = math.nan
    median_rtt_us: float = math.nan


class HostClockModel:
    def __init__(self, window: int = 16) -> None:
        self._samples: deque[ClockExchange] = deque(maxlen=window)
        self._quality = HostClockQuality(valid=False, points=0)

    def add(self, sample: ClockExchange) -> HostClockQuality:
        self._samples.append(sample)
        if len(self._samples) < 4:
            self._quality = HostClockQuality(
                valid=False, points=len(self._samples))
            return self._quality

        values = list(self._samples)
        x_ref = values[-1].host_midpoint_ns
        y_ref = values[-1].korora_midpoint_ticks
        xs = [value.host_midpoint_ns - x_ref for value in values]
        ys = [value.korora_midpoint_ticks - y_ref for value in values]
        x_mean = statistics.fmean(xs)
        y_mean = statistics.fmean(ys)
        denominator = sum((value - x_mean) ** 2 for value in xs)
        if denominator <= 0:
            return self._quality
        slope = sum(
            (x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)
        ) / denominator
        relative_intercept = y_mean - slope * x_mean
        intercept = y_ref + relative_intercept - slope * x_ref
        residual_ns = [
            ((intercept + slope * value.host_midpoint_ns) - value.korora_midpoint_ticks)
            / slope
            for value in values
        ]
        rms_ns = math.sqrt(statistics.fmean(
            value * value for value in residual_ns))
        self._quality = HostClockQuality(
            valid=True,
            points=len(values),
            slope_ticks_per_ns=slope,
            intercept_ticks=intercept,
            rms_ns=rms_ns,
            median_rtt_us=statistics.median(
                value.network_rtt_ns for value in values) / 1000,
        )
        return self._quality

    @property
    def quality(self) -> HostClockQuality:
        return self._quality

    def reset(self) -> None:
        self._samples.clear()
        self._quality = HostClockQuality(valid=False, points=0)
