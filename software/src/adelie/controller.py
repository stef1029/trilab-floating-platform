from __future__ import annotations

from dataclasses import replace
from collections.abc import Callable
import random
import threading
import time
from pathlib import Path
from typing import Any

from .ble_link import BleLink
from .config import BoardAssignment, RigConfig
from .constants import (
    HOST_ADDRESS,
    Channel,
    CommandField,
    CommandFlag,
    DEFAULT_MAX_NODES,
    NODE_ADDRESS_BASE,
    Field,
    TTL_GENERATOR_ADDRESS,
    HUB_ADDRESS,
    MessageKind,
    Opcode,
    RecordType,
    Status,
    TelemetryLevel,
    ValueType,
)
from .health import NodeState, node_state
from .models import (
    ClockExchange,
    HostClockModel,
    InventoryEntry,
    LocalSensorState,
    NodeStats,
    TtlStats,
)
from .protocol import (
    CommandMessage,
    Record,
    decode_command,
    decode_record,
    decode_imu_samples,
    encode_command,
)
from .recorder import LogRecorder
from .transport import Message

EventCallback = Callable[[str, Any], None]


class AdelieController:
    def __init__(self, config_path: Path, on_event: EventCallback) -> None:
        self.config_path = config_path
        self.config = RigConfig.load(config_path)
        self.on_event = on_event
        self.recorder = LogRecorder()
        self.clock = HostClockModel()
        # Node boards the hub currently sees, by flashed node number.
        self.inventory: dict[int, InventoryEntry] = {}
        self.nodes: dict[int, NodeStats] = {
            HUB_ADDRESS: NodeStats(HUB_ADDRESS, "hub"),
            TTL_GENERATOR_ADDRESS: NodeStats(TTL_GENERATOR_ADDRESS, "ttl_generator"),
        }
        self.session_id = 0
        self.local_sensors = LocalSensorState()
        self.command_id = random.randint(1, 0x7FFF_FFFF)
        self.connected = False
        self.telemetry_level = TelemetryLevel.STANDARD
        self._pending_clock: dict[int, int] = {}
        self._start_session_command: int | None = None
        self._stop_session_command: int | None = None
        self._stop_timer: threading.Timer | None = None
        self._stop_reason = "record stop"
        self._ttl_sequences: dict[tuple[int, int], TtlStats] = {}
        self._last_diagnostic_log_ns = 0
        self._clock_exchange_count = 0
        self._last_clock_exchange_ns = 0
        self._last_clock_exchange_rtt_us = float("nan")
        self._lock = threading.RLock()
        self.link = BleLink(
            on_message=self._on_message,
            on_state=self._on_state,
            on_sent=self._on_transport_sent,
            on_write_complete=self._on_transport_write_complete,
        )
        self._name_nodes()

    def connect(self) -> None:
        self.link.start()

    @property
    def ready_to_record(self) -> bool:
        if not self.connected or not self.config.assignments:
            return False
        required = [
            NODE_ADDRESS_BASE + assignment.node_number
            for assignment in self.config.assignments
        ]
        # node_state() also requires a recent record, because the
        # synchronized flag is sticky once a board has gone quiet.
        return all(
            node_state(self.nodes.get(address)) is NodeState.READY
            for address in [TTL_GENERATOR_ADDRESS, *required]
        )

    def disconnect(self) -> None:
        self.link.stop()
        self._end_run("manual disconnect")

    def _next_command_id(self) -> int:
        with self._lock:
            value = self.command_id
            self.command_id = 1 if value == 0xFFFF_FFFF else value + 1
            return value

    def command(
        self,
        opcode: Opcode,
        *,
        destination: int = HUB_ADDRESS,
        fields: list[tuple[int, ValueType, Any]] | None = None,
        execute_at_ticks: int = 0,
        flags: CommandFlag = CommandFlag.REQUIRE_RESPONSE,
        deadline_ms: int = 2000,
    ) -> int:
        if not self.connected:
            self.on_event(
                "warning", "command ignored because Hub is disconnected")
            return 0
        command_id = self._next_command_id()
        command_fields = fields or []
        payload = encode_command(
            kind=MessageKind.COMMAND,
            opcode=opcode,
            command_id=command_id,
            session_id=self.session_id,
            execute_at_ticks=execute_at_ticks,
            deadline_ms=deadline_ms,
            flags=flags,
            fields=command_fields,
        )
        queued_ns = time.perf_counter_ns()
        transfer_id = self.link.send(
            channel=Channel.COMMAND,
            destination=destination,
            payload=payload,
        )
        self.recorder.command_queued(
            queued_monotonic_ns=queued_ns,
            transfer_id=transfer_id,
            command_id=command_id,
            opcode=int(opcode),
            opcode_name=opcode.name,
            destination=destination,
            session_id=self.session_id,
            execute_at_ticks=execute_at_ticks,
            deadline_ms=deadline_ms,
            fields=command_fields,
        )
        return command_id

    def _on_transport_sent(self, transfer_id: int, sent_ns: int) -> None:
        self.recorder.transport_sent(
            transfer_id=transfer_id,
            sent_monotonic_ns=sent_ns,
        )

    def _on_transport_write_complete(
        self, transfer_id: int, completed_ns: int
    ) -> None:
        self.recorder.transport_write_complete(
            transfer_id=transfer_id,
            completed_monotonic_ns=completed_ns,
        )

    def request_inventory(self) -> None:
        self.command(Opcode.GET_INVENTORY)

    def set_telemetry(self, level: TelemetryLevel) -> None:
        self.telemetry_level = level
        if not self.connected:
            return
        self.command(
            Opcode.SET_TELEMETRY,
            fields=[
                (CommandField.TELEMETRY_LEVEL, ValueType.U8, int(level)),
            ],
        )

    def identify(self, entry: InventoryEntry, duration_ms: int = 3000) -> None:
        self.command(
            Opcode.IDENTIFY,
            destination=entry.address,
            fields=[
                (CommandField.IDENTIFY_DURATION_MS, ValueType.U32, duration_ms),
            ],
        )

    def save_assignments(self, assignments: list[BoardAssignment]) -> None:
        candidate = RigConfig(
            assignments=[value.normalized() for value in assignments]
        )
        candidate.validate()
        candidate.save(self.config_path)
        self.config = candidate
        self._name_nodes()
        self._publish_inventory()

    def start_recording(self, path: Path) -> None:
        if not self.connected:
            raise RuntimeError("Hub is not connected")
        if not self.config.assignments:
            raise RuntimeError("no node boards are configured")
        if not self.ready_to_record:
            raise RuntimeError(
                "wait for every node and TTL generator clock model")
        self.session_id = random.randint(1, 0xFFFF_FFFE)
        self.local_sensors.raw_chunks_received = 0
        self.local_sensors.raw_samples_received = 0
        self.local_sensors.last_chunk_sequence = 0
        self.recorder.start(
            path,
            self.session_id,
            {
                "protocol": {
                    "transport": 1, "record": 3, "command": 2, "node_identity": 1,
                },
                # The UUID records which physical board was in each port; it
                # is None if the hub has not seen that board's boot record.
                "assignments": [
                    {
                        "uuid": self._uuid_of(item.node_number),
                        "node_number": item.node_number,
                        "label": item.label,
                    }
                    for item in self.config.assignments
                ],
            },
        )
        self._start_session_command = self.command(
            Opcode.START_SESSION,
            flags=CommandFlag.REQUIRE_RESPONSE | CommandFlag.EXECUTE_IMMEDIATELY,
        )
        self.on_event("recording", {"active": True, "path": str(path)})

    def stop_recording(self) -> None:
        self._request_stop("record stop")

    def _request_stop(self, reason: str) -> None:
        if not self.session_id or self._stop_session_command is not None:
            return
        self._stop_reason = reason
        self.recorder.marker("stop_requested")
        self._stop_session_command = self.command(Opcode.STOP_SESSION)
        self._schedule_stop_finish(2.0)

    def _schedule_stop_finish(self, delay_s: float) -> None:
        if self._stop_timer is not None:
            self._stop_timer.cancel()
        self._stop_timer = threading.Timer(
            delay_s, lambda: self._end_run(self._stop_reason)
        )
        self._stop_timer.daemon = True
        self._stop_timer.start()

    def start_ttl(self, frequency_hz: float, width_us: int) -> None:
        self.command(
            Opcode.START_TTL_TRAIN,
            fields=[
                (
                    CommandField.TTL_FREQUENCY_MILLIHZ,
                    ValueType.U32,
                    round(frequency_hz * 1000),
                ),
                (CommandField.TTL_WIDTH_US, ValueType.U32, width_us),
                (CommandField.TTL_COUNT, ValueType.U32, 0),
            ],
        )

    def stop_ttl(self) -> None:
        self.command(Opcode.STOP_TTL_TRAIN)

    def start_sync_test(self) -> None:
        self.command(
            Opcode.START_SYNC_TEST,
            fields=[
                (CommandField.TTL_FREQUENCY_MILLIHZ, ValueType.U32, 1000),
                (CommandField.TTL_WIDTH_US, ValueType.U32, 100),
                (CommandField.TEST_COMMAND_INTERVAL_MS, ValueType.U32, 5000),
            ],
        )

    def stop_sync_test(self) -> None:
        self.command(Opcode.STOP_SYNC_TEST)

    def set_rgb(self, address: int, red: int, green: int, blue: int) -> None:
        self.command(
            Opcode.SET_RGB,
            destination=address,
            fields=[
                (CommandField.RED, ValueType.U8, red),
                (CommandField.GREEN, ValueType.U8, green),
                (CommandField.BLUE, ValueType.U8, blue),
                (CommandField.DURATION_MS, ValueType.U32, 0),
            ],
        )

    def set_ir(self, address: int, enabled: bool) -> None:
        self.command(
            Opcode.SET_IR,
            destination=address,
            fields=[
                (CommandField.ENABLED, ValueType.U8, int(enabled)),
            ],
        )

    def set_status_led(self, led_index: int, enabled: bool) -> None:
        # Protocol uses schematic LED numbers. D3 is firmware-owned READY;
        # D1 and D2 map to nPM1300 LED2 and LED1 respectively.
        if led_index not in (1, 2):
            raise ValueError("only programmable hub LEDs 1 and 2 may be set")
        self.command(
            Opcode.SET_STATUS_LED,
            fields=[
                (CommandField.LED_INDEX, ValueType.U8, led_index),
                (CommandField.ENABLED, ValueType.U8, int(enabled)),
            ],
        )

    def set_audio(
        self,
        address: int,
        *,
        mode: int,
        frequency_hz: int,
        low_hz: int,
        high_hz: int,
        amplitude: int,
        duration_ms: int,
    ) -> None:
        self.command(
            Opcode.SET_AUDIO,
            destination=address,
            fields=[
                (CommandField.AUDIO_MODE, ValueType.U8, mode),
                (CommandField.FREQUENCY_HZ, ValueType.U32, frequency_hz),
                (CommandField.LOW_FREQUENCY_HZ, ValueType.U32, low_hz),
                (CommandField.HIGH_FREQUENCY_HZ, ValueType.U32, high_hz),
                (CommandField.AMPLITUDE, ValueType.U16, amplitude),
                (CommandField.DURATION_MS, ValueType.U32, duration_ms),
            ],
        )

    def valve(self, address: int, duration_ms: int) -> None:
        self.command(
            Opcode.ACTUATE_VALVE,
            destination=address,
            flags=CommandFlag.REQUIRE_RESPONSE
            | CommandFlag.EXECUTE_IMMEDIATELY,
            fields=[
                (CommandField.DURATION_MS, ValueType.U32, duration_ms),
            ],
        )

    def configure_valves(self) -> None:
        for assignment in self.config.assignments:
            self.configure_valve(NODE_ADDRESS_BASE + assignment.node_number)

    def configure_valve(self, address: int) -> None:
        sensible_defaults = [
            (CommandField.VLOAD_MILLIVOLTS, ValueType.U32, 5000),
            (CommandField.SPIKE_DURATION_US, ValueType.U32, 12_000),
            (CommandField.SPIKE_DUTY_PER_MILLE, ValueType.U16, 660),
            (CommandField.HOLD_DUTY_PER_MILLE, ValueType.U16, 400),
            (CommandField.MAX_ON_TIME_US, ValueType.U32, 250_000),
            (CommandField.MINIMUM_INTERVAL_US, ValueType.U32, 250_000),
        ]
        self.command(
            Opcode.CONFIGURE_VALVE,
            destination=address,
            fields=sensible_defaults,
        )

    def clock_exchange(self) -> None:
        if not self.connected:
            return
        t1 = time.perf_counter_ns()
        command_id = self.command(
            Opcode.CLOCK_EXCHANGE,
            fields=[(CommandField.CLOCK_T1_NS, ValueType.U64, t1)],
        )
        self._pending_clock[command_id] = t1
        while len(self._pending_clock) > 8:
            self._pending_clock.pop(next(iter(self._pending_clock)))

    def _on_state(self, state: str, detail: str) -> None:
        if state == "warning":
            self.on_event("warning", detail)
            return
        with self._lock:
            if state == "connected":
                self.connected = True
                self.clock.reset()
                self._pending_clock.clear()
                self._clock_exchange_count = 0
                self._last_clock_exchange_ns = 0
                self._last_clock_exchange_rtt_us = float("nan")
            if state in {"disconnected", "error", "stopped"}:
                self.connected = False
                self.local_sensors = LocalSensorState()
                self.nodes[HUB_ADDRESS].connected = False
                self.inventory.clear()
                self._publish_inventory()
                self._end_run(f"BLE {state}")
            if state == "connected":
                self.nodes[HUB_ADDRESS].connected = True
                self.request_inventory()
                self.set_telemetry(self.telemetry_level)
        self.on_event("connection", {"state": state, "detail": detail})

    def _on_message(self, message: Message, received_ns: int) -> None:
        try:
            if message.channel is Channel.RECORD:
                record = decode_record(message.payload)
                self._handle_record(message, record, received_ns)
            elif message.channel is Channel.COMMAND:
                response = decode_command(message.payload)
                self._handle_response(message, response, received_ns)
        except Exception as error:
            self.on_event("warning", f"dropped application message: {error}")

    def _handle_record(
        self, message: Message, record: Record, received_ns: int
    ) -> None:
        address = message.source
        stats = self.nodes.setdefault(
            address, NodeStats(address, f"node_{address:02x}")
        )
        stats.connected = True
        stats.note_record(received_ns)
        fields = record.field_map

        if record.record_type is RecordType.SYNC_QUALITY:
            stats.synchronized = bool(record.flags & 0x04)
            stats.rms_ns = float(fields.get(int(Field.RMS_NS), stats.rms_ns))
            stats.skew_ppb = int(fields.get(
                int(Field.SKEW_PPB), stats.skew_ppb))
            stats.model_points = int(
                fields.get(int(Field.MODEL_POINTS), stats.model_points)
            )
        elif record.record_type in (RecordType.HEALTH, RecordType.LINK_QUALITY):
            stats.queue_depth = int(
                fields.get(int(Field.QUEUE_DEPTH), stats.queue_depth)
            )
            stats.dropped_records = int(
                fields.get(int(Field.DROPPED_RECORDS), stats.dropped_records)
            )
            stats.transport_errors = int(
                fields.get(int(Field.TRANSPORT_ERRORS), stats.transport_errors)
            )
            stats.retries = int(fields.get(
                int(Field.RETRY_COUNT), stats.retries))
            stats.timeout_count = int(
                fields.get(int(Field.TIMEOUT_COUNT), stats.timeout_count)
            )
            stats.decode_errors = int(
                fields.get(
                    int(Field.TRANSPORT_DECODE_ERRORS), stats.decode_errors
                )
            )
            stats.reassembly_errors = int(
                fields.get(
                    int(Field.TRANSPORT_REASSEMBLY_ERRORS),
                    stats.reassembly_errors,
                )
            )
            stats.transmit_errors = int(
                fields.get(
                    int(Field.TRANSPORT_TRANSMIT_ERRORS),
                    stats.transmit_errors,
                )
            )
            stats.ttl_capture_count = int(
                fields.get(
                    int(Field.TTL_CAPTURE_COUNT), stats.ttl_capture_count
                )
            )
            stats.ttl_capture_drops = int(
                fields.get(
                    int(Field.TTL_CAPTURE_DROPS), stats.ttl_capture_drops
                )
            )
            if int(Field.RSSI_DBM) in fields:
                stats.rssi_dbm = int(fields[int(Field.RSSI_DBM)])
        elif record.record_type is RecordType.LOCAL_SENSORS:
            self._update_local_sensors(fields, received_ns)
        elif record.record_type is RecordType.IMU_SAMPLES:
            self._update_imu_samples(record, fields, received_ns)
        elif record.record_type is RecordType.POWER_STATUS:
            self._update_power_status(fields, received_ns)
        elif record.record_type is RecordType.LIGHT_GATE:
            stats.note_light_gate(received_ns)
        elif record.record_type in {
            RecordType.TTL_SCHEDULED,
            RecordType.TTL_GENERATED,
            RecordType.TTL_CAPTURED,
            RecordType.TTL_RESULT,
        }:
            self._update_ttl(record, fields, received_ns)
        elif record.record_type is RecordType.INVENTORY:
            self._update_inventory_record(record)
        elif record.record_type is RecordType.FAULT and self.session_id:
            self._request_stop(
                str(fields.get(int(Field.REASON), "critical hardware fault"))
            )

        self.recorder.record(
            received_monotonic_ns=received_ns,
            transport_message=message,
            record=record,
        )
        self.on_event(
            "record",
            {"message": message, "record": record, "stats": stats},
        )

    def _update_local_sensors(
        self, fields: dict[int, Any], received_ns: int
    ) -> None:
        status = int(fields.get(int(Field.SENSOR_STATUS), 0))
        sensors = self.local_sensors
        sensors.i2c_ready = bool(status & 0x01)
        sensors.imu_present = bool(status & 0x02)
        sensors.magnetometer_present = bool(status & 0x04)
        sensors.pmic_present = bool(status & 0x08) or sensors.pmic_present
        sensors.sample_rate_hz = int(
            fields.get(int(Field.SAMPLE_RATE_HZ), sensors.sample_rate_hz)
        )
        sensors.sample_count = int(
            fields.get(int(Field.SAMPLE_COUNT), sensors.sample_count)
        )
        sensors.i2c_errors = int(
            fields.get(int(Field.SENSOR_I2C_ERRORS), sensors.i2c_errors)
        )
        sensors.accel_mg = tuple(
            int(fields.get(int(tag), current))
            for tag, current in zip(
                (Field.ACCEL_X_MG, Field.ACCEL_Y_MG, Field.ACCEL_Z_MG),
                sensors.accel_mg,
            )
        )
        sensors.gyro_mdps = tuple(
            int(fields.get(int(tag), current))
            for tag, current in zip(
                (Field.GYRO_X_MDPS, Field.GYRO_Y_MDPS, Field.GYRO_Z_MDPS),
                sensors.gyro_mdps,
            )
        )
        sensors.mag_milligauss = tuple(
            int(fields.get(int(tag), current))
            for tag, current in zip(
                (
                    Field.MAG_X_MILLIGAUSS,
                    Field.MAG_Y_MILLIGAUSS,
                    Field.MAG_Z_MILLIGAUSS,
                ),
                sensors.mag_milligauss,
            )
        )
        sensors.last_update_ns = received_ns
        self.on_event("local_sensors", sensors)

    def _update_imu_samples(
        self, record: Record, fields: dict[int, Any], received_ns: int
    ) -> None:
        samples = decode_imu_samples(record)
        sensors = self.local_sensors
        sensors.raw_chunks_received += 1
        sensors.raw_samples_received += len(samples)
        sensors.last_chunk_sequence = int(
            fields.get(int(Field.CHUNK_SEQUENCE), sensors.last_chunk_sequence)
        )
        sensors.last_update_ns = received_ns

    def _update_power_status(
        self, fields: dict[int, Any], received_ns: int
    ) -> None:
        sensors = self.local_sensors
        sensors.pmic_present = bool(
            fields.get(int(Field.PMIC_PRESENT), sensors.pmic_present)
        )
        sensors.power_source = str(
            fields.get(int(Field.POWER_SOURCE), sensors.power_source)
        )
        optional_fields = (
            (Field.SUPPLY_MILLIVOLTS, "supply_millivolts"),
            (Field.BATTERY_MILLIVOLTS, "battery_millivolts"),
            (Field.BATTERY_CURRENT_MA, "battery_current_ma"),
            (Field.BATTERY_SOC_PER_MILLE, "battery_soc_per_mille"),
            (Field.BATTERY_HEALTH_PER_MILLE, "battery_health_per_mille"),
        )
        for tag, attribute in optional_fields:
            if int(tag) in fields:
                setattr(sensors, attribute, int(fields[int(tag)]))
        if int(Field.DETAIL) in fields:
            sensors.charge_status = str(fields[int(Field.DETAIL)])
        else:
            sensors.charge_status = ""
        if int(Field.REASON) in fields:
            sensors.charger_error = str(fields[int(Field.REASON)])
        else:
            sensors.charger_error = ""
        sensors.last_update_ns = received_ns
        self.on_event("local_sensors", sensors)

    def _update_ttl(
        self, record: Record, fields: dict[int, Any], received_ns: int
    ) -> None:
        sequence_value = fields.get(int(Field.SEQUENCE))
        if sequence_value is None:
            return
        sequence = int(sequence_value)
        key = (record.session_id, sequence)
        ttl = self._ttl_sequences.setdefault(
            key, TtlStats(session_id=record.session_id, sequence=sequence)
        )
        ttl.received_ns = received_ns
        ttl.latest_record = record.record_type.name.lower()

        requested = fields.get(int(Field.REQUESTED_TICKS))
        actual = fields.get(int(Field.ACTUAL_TICKS))
        if record.record_type is RecordType.TTL_SCHEDULED:
            ttl.target_hub_ticks = int(
                requested if requested is not None else record.timestamp_ticks
            )
        elif record.record_type is RecordType.TTL_GENERATED:
            if requested is not None:
                ttl.generated_target_local_ticks = int(requested)
            ttl.generated_local_ticks = int(
                actual if actual is not None else record.timestamp_ticks
            )
            reference = fields.get(int(Field.REFERENCE_TICKS))
            if reference is not None:
                ttl.generated_hub_ticks = int(reference)
            self.nodes[TTL_GENERATOR_ADDRESS].note_ttl(sequence, received_ns)
        elif record.record_type is RecordType.TTL_CAPTURED:
            if requested is not None:
                ttl.target_hub_ticks = int(requested)
            ttl.captured_hub_ticks = int(
                actual if actual is not None else record.timestamp_ticks
            )
            self.nodes[HUB_ADDRESS].note_ttl(sequence, received_ns)
        elif record.record_type is RecordType.TTL_RESULT:
            if requested is not None:
                ttl.target_hub_ticks = int(requested)
            if actual is not None:
                ttl.captured_hub_ticks = int(actual)
            result_ns = fields.get(int(Field.VALUE))
            if result_ns is not None:
                ttl.result_error_us = float(result_ns) / 1000.0

        ttl.recompute()
        ttl_generator = self.nodes[TTL_GENERATOR_ADDRESS]
        if ttl.output_error_us == ttl.output_error_us:
            ttl_generator.last_ttl_sequence = sequence
            ttl_generator.last_ttl_error_us = ttl.output_error_us
            ttl_generator.last_ttl_ns = received_ns
        hub = self.nodes[HUB_ADDRESS]
        input_error = (
            ttl.result_error_us
            if ttl.result_error_us == ttl.result_error_us
            else ttl.input_error_us
        )
        if input_error == input_error:
            hub.last_ttl_sequence = sequence
            hub.last_ttl_error_us = input_error
            hub.last_ttl_ns = received_ns

        self.on_event("ttl", replace(ttl))
        while len(self._ttl_sequences) > 64:
            self._ttl_sequences.pop(next(iter(self._ttl_sequences)))

    def live_diagnostics(self) -> dict[str, Any]:
        now_ns = time.perf_counter_ns()
        link = self.link.diagnostics()
        clock = self.clock.quality
        hub = self.nodes[HUB_ADDRESS]
        values: dict[str, Any] = {
            "sample_ns": now_ns,
            "ble_connected": link.connected,
            "att_write_size": link.att_write_size,
            "fragment_payload": link.fragment_payload,
            "rx_notifications": link.rx_notifications,
            "rx_frames": link.rx_frames,
            "rx_messages": link.rx_messages,
            "rx_bytes": link.rx_bytes,
            "rx_decode_errors": link.rx_decode_errors,
            "rx_reassembly_errors": link.rx_reassembly_errors,
            "rx_address_errors": link.rx_address_errors,
            "tx_messages": link.tx_messages,
            "tx_fragments": link.tx_fragments,
            "tx_bytes": link.tx_bytes,
            "tx_write_errors": link.tx_write_errors,
            "outgoing_queue": link.outgoing_queue,
            "last_rx_age_ms": (
                (now_ns - link.last_rx_ns) / 1e6 if link.last_rx_ns else None
            ),
            "last_tx_age_ms": (
                (now_ns - link.last_tx_ns) / 1e6 if link.last_tx_ns else None
            ),
            "last_error": link.last_error,
            "clock_valid": clock.valid,
            "clock_points": clock.points,
            "clock_rms_us": clock.rms_ns / 1000.0,
            "clock_median_rtt_us": clock.median_rtt_us,
            "clock_skew_ppm": (
                (clock.slope_ticks_per_ns / 0.016 - 1.0) * 1e6
                if clock.valid else float("nan")
            ),
            "clock_exchange_count": self._clock_exchange_count,
            "clock_last_exchange_ns": self._last_clock_exchange_ns,
            "clock_last_rtt_us": self._last_clock_exchange_rtt_us,
            # This counter comes from hub_rs485::errors(), not the host BLE
            # link. Keeping it explicitly named prevents misleading diagnosis.
            "hub_rs485_errors": hub.transport_errors,
            "hub_rs485_retries": hub.retries,
            "hub_rs485_timeouts": hub.timeout_count,
            "hub_rs485_decode_errors": hub.decode_errors,
            "hub_rs485_reassembly_errors": hub.reassembly_errors,
            "hub_rs485_transmit_errors": hub.transmit_errors,
            "hub_ble_dropped": hub.dropped_records,
            "ttl_generator_ttl_generated": self.nodes[
                TTL_GENERATOR_ADDRESS
            ].ttl_events,
            "hub_ttl_captured_records": hub.ttl_events,
            "hub_ttl_capture_edges": hub.ttl_capture_count,
            "hub_ttl_capture_drops": hub.ttl_capture_drops,
        }
        if (
            self.recorder.active
            and now_ns - self._last_diagnostic_log_ns >= 1_000_000_000
        ):
            self._last_diagnostic_log_ns = now_ns
            self.recorder.marker("host_transport_health", **values)
        return values

    def _handle_response(
        self, message: Message, response: CommandMessage, received_ns: int
    ) -> None:
        self.recorder.command_response(
            received_monotonic_ns=received_ns,
            transport_message=message,
            response=response,
        )
        if response.opcode is Opcode.CLOCK_EXCHANGE:
            t1 = self._pending_clock.pop(response.command_id, None)
            fields = {field.tag: field.value for field in response.fields}
            if t1 is not None:
                t2 = fields.get(int(CommandField.CLOCK_T2_TICKS))
                t3 = fields.get(int(CommandField.CLOCK_T3_TICKS))
                if t2 is not None and t3 is not None:
                    exchange = ClockExchange(
                        t1, int(t2), int(t3), received_ns
                    )
                    quality = self.clock.add(exchange)
                    self._clock_exchange_count += 1
                    self._last_clock_exchange_ns = received_ns
                    self._last_clock_exchange_rtt_us = (
                        exchange.network_rtt_ns / 1000.0
                    )
                    self.on_event("host_clock", quality)
        elif response.opcode is Opcode.GET_INVENTORY:
            self._decode_inventory_response(response)
        elif (
            response.opcode is Opcode.START_SESSION
            and response.command_id == self._start_session_command
        ):
            self._start_session_command = None
            if response.status not in (Status.OK, Status.ACCEPTED):
                self._end_run(
                    f"session start rejected: {response.status.name}")
        elif (
            response.opcode is Opcode.STOP_SESSION
            and response.command_id == self._stop_session_command
        ):
            self._stop_session_command = None
            self._schedule_stop_finish(0.75)

        self.on_event(
            "response",
            {"source": message.source, "response": response},
        )

    # ---- node inventory ------------------------------------------------
    #
    # Node boards are numbered when flashed and node N always answers at
    # NODE_ADDRESS_BASE + N, so the inventory is simply which numbers the hub
    # currently sees. The hub sends an INVENTORY record whenever a node
    # appears, disappears or restarts, and GET_INVENTORY returns the full set.

    def _name_nodes(self) -> None:
        """Give every configured node's stats card its configured name."""
        for assignment in self.config.assignments:
            address = NODE_ADDRESS_BASE + assignment.node_number
            stats = self.nodes.setdefault(address, NodeStats(address, assignment.name))
            stats.name = assignment.name

    def _uuid_of(self, node_number: int) -> str | None:
        entry = self.inventory.get(node_number)
        return entry.uuid if entry is not None else None

    def _publish_inventory(self) -> None:
        present = set(self.inventory)
        configured = set(self.config.node_numbers)
        self.on_event(
            "inventory",
            {
                "entries": [self.inventory[number] for number in sorted(present)],
                "missing": sorted(configured - present),
                "unexpected": sorted(present - configured),
            },
        )

    def _node_present(
        self, address: int, uuid_bytes: bytes | None, capabilities: int
    ) -> bool:
        """Record a present node. Returns False for an address that is not a
        node slot."""
        node_number = address - NODE_ADDRESS_BASE
        if not 0 <= node_number < DEFAULT_MAX_NODES:
            return False
        uuid = (
            uuid_bytes.hex()
            if uuid_bytes and any(uuid_bytes)
            else None
        )
        previous = self.inventory.get(node_number)
        self.inventory[node_number] = InventoryEntry(
            node_number=node_number,
            address=address,
            capabilities=capabilities,
            uuid=uuid or (previous.uuid if previous else None),
        )
        # Valve limits live in node RAM, so a node that has just appeared or
        # restarted needs them again.
        self.configure_valve(address)
        return True

    def _decode_inventory_response(self, response: CommandMessage) -> None:
        raw = next(
            (
                field.value
                for field in response.fields
                if field.tag == int(CommandField.UUID_LIST)
            ),
            b"",
        )
        if not isinstance(raw, bytes) or len(raw) % 18:
            return
        # 18 bytes per present node: UUID, address, node number, capabilities.
        self.inventory.clear()
        for offset in range(0, len(raw), 18):
            self._node_present(
                address=raw[offset + 12],
                uuid_bytes=raw[offset: offset + 12],
                capabilities=int.from_bytes(
                    raw[offset + 14: offset + 18], "little"),
            )
        self._publish_inventory()

    def _update_inventory_record(self, record: Record) -> None:
        fields = record.field_map
        address = fields.get(int(Field.LINK_ADDRESS))
        if address is None:
            return
        address = int(address)
        node_number = address - NODE_ADDRESS_BASE
        required = node_number in self.config.node_numbers
        if fields.get(int(Field.STATE), True) is False:
            self.inventory.pop(node_number, None)
            if self.session_id and required:
                self._request_stop("Node disconnected")
        else:
            # The hub only reports a present node when it appears or its
            # board restarts; a restart of a node already present means it
            # has left the session.
            restarted = node_number in self.inventory
            raw_uuid = fields.get(int(Field.UUID))
            if not self._node_present(
                address,
                raw_uuid if isinstance(raw_uuid, bytes) else None,
                int(fields.get(int(Field.VALUE), 0)),
            ):
                return
            if restarted and self.session_id and required:
                self._request_stop("Node restarted")
        self._publish_inventory()

    def _end_run(self, reason: str) -> None:
        if self._stop_timer is not None:
            self._stop_timer.cancel()
            self._stop_timer = None
        if self.recorder.active:
            path = self.recorder.stop(reason)
            self.on_event(
                "recording", {"active": False,
                              "path": str(path), "reason": reason}
            )
        self.session_id = 0
        self._start_session_command = None
        self._stop_session_command = None
        self._stop_reason = "record stop"
