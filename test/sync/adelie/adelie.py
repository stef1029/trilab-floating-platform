#!/usr/bin/env python3
from __future__ import annotations

import argparse
import asyncio
import csv
import math
import statistics
import struct
import sys
import time
import traceback

from collections import deque
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

from bleak import BleakClient, BleakScanner


DEVICE_NAME = "korora"

SERVICE_UUID = "a88279d0-7009-4bee-a6f8-e1dc3ff02b92"
RX_UUID = "a88279d1-7009-4bee-a6f8-e1dc3ff02b92"
TX_UUID = "a88279d2-7009-4bee-a6f8-e1dc3ff02b92"

MAGIC = 0xAD1E
VERSION = 1

FRAME = struct.Struct("<HBBIQI")
FRAME_SIZE = FRAME.size

CLOCK_SYNC_REQUEST = 0x01
REWARD_DO_NOW_REQUEST = 0x02

CLOCK_REPLY = 0x81

COMMAND_KORORA_RX = 0x90
COMMAND_I2C_TX_START = 0x91
COMMAND_REWARD_RX = 0x92
COMMAND_REWARD_EXEC = 0x93
COMMAND_KORORA_ACK_RX = 0x94
COMMAND_KORORA_DONE_TX = 0x95

ERROR = 0xFF

STATUS_OK = 0

HUB_HZ = 16_000_000.0

MODEL_WINDOW_SIZE = 16
DEFAULT_SYNC_INTERVAL_S = 1.0


CLOCK_FIELDS = [
    "sequence",
    "t1_adelie_ns",
    "t2_korora_ticks",
    "t3_korora_ticks",
    "t4_adelie_ns",
    "network_rtt_ns",
    "full_exchange_us",
    "midpoint_adelie_ns",
    "midpoint_korora_ticks",
]


COMMAND_FIELDS = [
    "sequence",
    "model_generation",
    "model_points",
    "model_slope_error_ppm",
    "model_min_network_rtt_us",
    "model_median_network_rtt_us",
    "adelie_t1_ns",
    "adelie_t7_ns",
    "total_rtt_us",
    "ble_down_est_us",
    "ble_up_est_us",
    "korora_rx_ticks",
    "i2c_tx_start_ticks",
    "reward_rx_hub_ticks",
    "reward_exec_hub_ticks",
    "korora_ack_rx_ticks",
    "korora_done_tx_ticks",
    "korora_queue_to_i2c_us",
    "korora_post_ack_us",
    "reward_model_valid",
    "i2c_down_us",
    "reward_action_us",
    "i2c_up_and_poll_us",
    "korora_reward_rtt_us",
    "korora_internal_us",
]


def log(message: str) -> None:
    print(message, flush=True)


@dataclass(frozen=True)
class Message:
    message_type: int
    sequence: int
    timestamp: int
    value: int
    adelie_rx_ns: int


@dataclass(frozen=True)
class ClockSample:
    sequence: int
    t1_adelie_ns: int
    t2_korora_ticks: int
    t3_korora_ticks: int
    t4_adelie_ns: int
    network_rtt_ns: float
    full_exchange_us: float
    midpoint_adelie_ns: float
    midpoint_korora_ticks: float


@dataclass(frozen=True)
class ClockModel:
    slope_ticks_per_ns: float
    intercept_ticks: float
    sample_count: int
    median_network_rtt_us: float
    minimum_network_rtt_us: float

    def adelie_ns_to_korora_ticks(
        self,
        adelie_ns: int | float,
    ) -> float:
        return self.intercept_ticks + self.slope_ticks_per_ns * float(adelie_ns)

    def korora_ticks_to_adelie_ns(
        self,
        ticks: int | float,
    ) -> float:
        return (float(ticks) - self.intercept_ticks) / self.slope_ticks_per_ns

    @property
    def slope_error_ppm(self) -> float:
        expected_slope = HUB_HZ / 1_000_000_000.0

        return (self.slope_ticks_per_ns / expected_slope - 1.0) * 1_000_000.0


class CsvAppender:
    def __init__(
        self,
        path: Path,
        fieldnames: list[str],
    ) -> None:
        path.parent.mkdir(
            parents=True,
            exist_ok=True,
        )

        self.path = path.resolve()

        self._handle = path.open(
            "w",
            newline="",
            encoding="utf-8",
        )

        self._writer = csv.DictWriter(
            self._handle,
            fieldnames=fieldnames,
        )

        self._writer.writeheader()
        self._handle.flush()

    def append(
        self,
        row: dict[str, object],
    ) -> None:
        self._writer.writerow(row)
        self._handle.flush()

    def close(self) -> None:
        self._handle.close()


class Inbox:
    def __init__(
        self,
        verbose: bool,
    ) -> None:
        self._queue: asyncio.Queue[Message] = asyncio.Queue()
        self._deferred: list[Message] = []
        self._verbose = verbose

    def callback(
        self,
        _sender: object,
        data: bytearray,
    ) -> None:
        received_ns = time.perf_counter_ns()

        if len(data) != FRAME_SIZE:
            log(
                "adelie: ignored notification "
                f"length={len(data)}, "
                f"expected={FRAME_SIZE}"
            )
            return

        (
            magic,
            version,
            message_type,
            sequence,
            timestamp,
            value,
        ) = FRAME.unpack(data)

        if magic != MAGIC or version != VERSION:
            log(
                "adelie: ignored malformed notification "
                f"magic=0x{magic:04x}, "
                f"version={version}"
            )
            return

        if self._verbose:
            log(
                "adelie: notify "
                f"type=0x{message_type:02x} "
                f"seq={sequence} "
                f"timestamp={timestamp} "
                f"value={value}"
            )

        self._queue.put_nowait(
            Message(
                message_type=message_type,
                sequence=sequence,
                timestamp=timestamp,
                value=value,
                adelie_rx_ns=received_ns,
            )
        )

    async def collect(
        self,
        sequence: int,
        expected_types: Iterable[int],
        timeout: float,
    ) -> dict[int, Message]:
        expected = set(expected_types)
        found: dict[int, Message] = {}

        remaining_deferred: list[Message] = []

        for message in self._deferred:
            if message.sequence == sequence and message.message_type in expected:
                found[message.message_type] = message
            else:
                remaining_deferred.append(message)

        self._deferred = remaining_deferred

        deadline = asyncio.get_running_loop().time() + timeout

        while set(found) != expected:
            remaining = deadline - asyncio.get_running_loop().time()

            if remaining <= 0:
                missing = sorted(expected - set(found))

                raise TimeoutError(
                    "timeout waiting for "
                    f"sequence {sequence}, types "
                    f"{[f'0x{x:02x}' for x in missing]}"
                )

            message = await asyncio.wait_for(
                self._queue.get(),
                timeout=remaining,
            )

            if message.sequence == sequence and message.message_type in expected:
                found[message.message_type] = message

            elif message.sequence == sequence and message.message_type == ERROR:
                raise RuntimeError(
                    f"korora error for sequence {sequence}: status={message.value}"
                )

            else:
                self._deferred.append(message)

        return found


class RollingClockTracker:
    def __init__(
        self,
        window_size: int,
    ) -> None:
        if window_size < 2:
            raise ValueError("window_size must be at least 2")

        self.window_size = window_size

        self.samples: deque[ClockSample] = deque(maxlen=window_size)

        self.model: ClockModel | None = None
        self.generation = 0

        self.ready = asyncio.Event()

        self._lock = asyncio.Lock()

    async def add(
        self,
        sample: ClockSample,
    ) -> tuple[ClockModel | None, int]:
        async with self._lock:
            self.samples.append(sample)

            if len(self.samples) < self.window_size:
                self.model = None
                self.ready.clear()

                return None, len(self.samples)

            self.model = fit_clock_model(list(self.samples))

            self.generation += 1
            self.ready.set()

            return self.model, len(self.samples)

    async def snapshot(
        self,
    ) -> tuple[ClockModel, int]:
        async with self._lock:
            if self.model is None:
                raise RuntimeError("Adelie clock model is not ready")

            return self.model, self.generation

    async def sample_count(self) -> int:
        async with self._lock:
            return len(self.samples)


def encode(
    message_type: int,
    sequence: int,
    timestamp: int,
    value: int = 0,
) -> bytes:
    return FRAME.pack(
        MAGIC,
        VERSION,
        message_type,
        sequence & 0xFFFFFFFF,
        timestamp & 0xFFFFFFFFFFFFFFFF,
        value & 0xFFFFFFFF,
    )


def fit_clock_model(
    samples: list[ClockSample],
) -> ClockModel:
    if len(samples) < 2:
        raise ValueError("at least two clock samples are required")

    x_reference = samples[-1].midpoint_adelie_ns
    y_reference = samples[-1].midpoint_korora_ticks

    x_values = [sample.midpoint_adelie_ns - x_reference for sample in samples]

    y_values = [sample.midpoint_korora_ticks - y_reference for sample in samples]

    x_mean = statistics.fmean(x_values)
    y_mean = statistics.fmean(y_values)

    denominator = sum((value - x_mean) ** 2 for value in x_values)

    if denominator <= 0.0:
        raise ValueError("degenerate clock samples")

    slope = (
        sum(
            (x_value - x_mean) * (y_value - y_mean)
            for x_value, y_value in zip(x_values, y_values)
        )
        / denominator
    )

    relative_intercept = y_mean - slope * x_mean

    intercept = y_reference + relative_intercept - slope * x_reference

    rtts_us = [sample.network_rtt_ns / 1_000.0 for sample in samples]

    return ClockModel(
        slope_ticks_per_ns=slope,
        intercept_ticks=intercept,
        sample_count=len(samples),
        median_network_rtt_us=statistics.median(rtts_us),
        minimum_network_rtt_us=min(rtts_us),
    )


async def exchange_clock(
    client: BleakClient,
    inbox: Inbox,
    sequence: int,
    timeout: float,
) -> ClockSample:
    t1 = time.perf_counter_ns()

    await asyncio.wait_for(
        client.write_gatt_char(
            RX_UUID,
            encode(
                CLOCK_SYNC_REQUEST,
                sequence,
                t1,
            ),
            response=True,
        ),
        timeout=timeout,
    )

    reply = (
        await inbox.collect(
            sequence,
            [CLOCK_REPLY],
            timeout,
        )
    )[CLOCK_REPLY]

    t4 = reply.adelie_rx_ns
    t2 = reply.timestamp
    t3 = t2 + reply.value

    server_processing_ns = reply.value * 1_000_000_000.0 / HUB_HZ

    network_rtt_ns = (t4 - t1) - server_processing_ns

    return ClockSample(
        sequence=sequence,
        t1_adelie_ns=t1,
        t2_korora_ticks=t2,
        t3_korora_ticks=t3,
        t4_adelie_ns=t4,
        network_rtt_ns=network_rtt_ns,
        full_exchange_us=(t4 - t1) / 1_000.0,
        midpoint_adelie_ns=(t1 + t4) / 2.0,
        midpoint_korora_ticks=(t2 + t3) / 2.0,
    )


async def run_command(
    client: BleakClient,
    inbox: Inbox,
    model: ClockModel,
    model_generation: int,
    sequence: int,
    timeout: float,
) -> dict[str, float | int]:
    t1 = time.perf_counter_ns()

    await asyncio.wait_for(
        client.write_gatt_char(
            RX_UUID,
            encode(
                REWARD_DO_NOW_REQUEST,
                sequence,
                t1,
                1,
            ),
            response=True,
        ),
        timeout=timeout,
    )

    expected = [
        COMMAND_KORORA_RX,
        COMMAND_I2C_TX_START,
        COMMAND_REWARD_RX,
        COMMAND_REWARD_EXEC,
        COMMAND_KORORA_ACK_RX,
        COMMAND_KORORA_DONE_TX,
    ]

    replies = await inbox.collect(
        sequence,
        expected,
        timeout,
    )

    done = replies[COMMAND_KORORA_DONE_TX]

    t7 = done.adelie_rx_ns

    korora_rx = replies[COMMAND_KORORA_RX].timestamp

    i2c_start = replies[COMMAND_I2C_TX_START].timestamp

    reward_rx = replies[COMMAND_REWARD_RX].timestamp

    reward_exec = replies[COMMAND_REWARD_EXEC].timestamp

    korora_ack = replies[COMMAND_KORORA_ACK_RX].timestamp

    korora_done = done.timestamp

    reward_model_valid = (
        replies[COMMAND_REWARD_RX].value == STATUS_OK
        and replies[COMMAND_REWARD_EXEC].value == STATUS_OK
        and reward_rx != 0
        and reward_exec != 0
    )

    t1_hub = model.adelie_ns_to_korora_ticks(t1)

    t7_hub = model.adelie_ns_to_korora_ticks(t7)

    tick_to_us = 1_000_000.0 / HUB_HZ

    row: dict[str, float | int] = {
        "sequence": sequence,
        "model_generation": model_generation,
        "model_points": model.sample_count,
        "model_slope_error_ppm": model.slope_error_ppm,
        "model_min_network_rtt_us": model.minimum_network_rtt_us,
        "model_median_network_rtt_us": model.median_network_rtt_us,
        "adelie_t1_ns": t1,
        "adelie_t7_ns": t7,
        "total_rtt_us": (t7 - t1) / 1_000.0,
        "ble_down_est_us": (korora_rx - t1_hub) * tick_to_us,
        "ble_up_est_us": (t7_hub - korora_done) * tick_to_us,
        "korora_rx_ticks": korora_rx,
        "i2c_tx_start_ticks": i2c_start,
        "reward_rx_hub_ticks": reward_rx,
        "reward_exec_hub_ticks": reward_exec,
        "korora_ack_rx_ticks": korora_ack,
        "korora_done_tx_ticks": korora_done,
        "korora_queue_to_i2c_us": (i2c_start - korora_rx) * tick_to_us,
        "korora_post_ack_us": (korora_done - korora_ack) * tick_to_us,
        "reward_model_valid": int(reward_model_valid),
    }

    if reward_model_valid:
        row.update(
            {
                "i2c_down_us": (reward_rx - i2c_start) * tick_to_us,
                "reward_action_us": (reward_exec - reward_rx) * tick_to_us,
                "i2c_up_and_poll_us": (korora_ack - reward_exec) * tick_to_us,
                "korora_reward_rtt_us": (korora_ack - i2c_start) * tick_to_us,
                "korora_internal_us": (korora_done - korora_rx) * tick_to_us,
            }
        )
    else:
        row.update(
            {
                "i2c_down_us": math.nan,
                "reward_action_us": math.nan,
                "i2c_up_and_poll_us": math.nan,
                "korora_reward_rtt_us": math.nan,
                "korora_internal_us": (korora_done - korora_rx) * tick_to_us,
            }
        )

    return row


async def clock_sync_worker(
    client: BleakClient,
    inbox: Inbox,
    tracker: RollingClockTracker,
    clock_writer: CsvAppender,
    protocol_lock: asyncio.Lock,
    timeout: float,
    sync_interval: float,
    stop_event: asyncio.Event,
    verbose: bool,
) -> None:
    sequence = 1

    loop = asyncio.get_running_loop()
    next_sync_time = loop.time()

    while not stop_event.is_set():
        try:
            async with protocol_lock:
                sample = await exchange_clock(
                    client=client,
                    inbox=inbox,
                    sequence=sequence,
                    timeout=timeout,
                )

            clock_writer.append(asdict(sample))

            model, sample_count = await tracker.add(sample)

            if model is None:
                log(
                    "adelie: clock acquire "
                    f"{sample_count}/"
                    f"{tracker.window_size} "
                    f"full="
                    f"{sample.full_exchange_us:.1f} us "
                    f"network="
                    f"{sample.network_rtt_ns / 1_000.0:.1f} us"
                )

            else:
                log(
                    "adelie: clock track "
                    f"generation="
                    f"{tracker.generation} "
                    f"points="
                    f"{model.sample_count} "
                    f"min network RTT="
                    f"{model.minimum_network_rtt_us:.1f} us "
                    f"median="
                    f"{model.median_network_rtt_us:.1f} us "
                    f"slope error="
                    f"{model.slope_error_ppm:.3f} ppm"
                )

            sequence = (sequence + 1) & 0xFFFFFFFF

            if sequence == 0:
                sequence = 1

        except asyncio.CancelledError:
            raise

        except Exception as error:
            log(f"adelie: clock sample failed: {type(error).__name__}: {error}")

            if verbose:
                traceback.print_exc()

        next_sync_time += sync_interval

        delay = max(
            0.0,
            next_sync_time - loop.time(),
        )

        try:
            await asyncio.wait_for(
                stop_event.wait(),
                timeout=delay,
            )

        except TimeoutError:
            pass


def print_services(
    client: BleakClient,
) -> None:
    log("adelie: discovered GATT services")

    for service in client.services:
        log(f"  service {service.uuid}")

        for characteristic in service.characteristics:
            log(
                f"    char {characteristic.uuid} "
                f"properties="
                f"{','.join(characteristic.properties)}"
            )


async def async_main(
    args: argparse.Namespace,
) -> int:
    if args.model_window < 2:
        raise ValueError("--model-window must be at least 2")

    if args.sync_interval <= 0:
        raise ValueError("--sync-interval must be positive")

    if args.commands < 0:
        raise ValueError("--commands cannot be negative")

    clock_writer = CsvAppender(
        args.clock_csv,
        CLOCK_FIELDS,
    )

    command_writer = CsvAppender(
        args.csv,
        COMMAND_FIELDS,
    )

    log(f"adelie: clock CSV is {clock_writer.path}")

    log(f"adelie: latency CSV is {command_writer.path}")

    log(f"adelie: scanning for {args.name!r}")

    clock_task: asyncio.Task[None] | None = None
    clock_stop = asyncio.Event()

    try:
        device = await BleakScanner.find_device_by_name(
            args.name,
            timeout=args.scan_timeout,
        )

        if device is None:
            raise RuntimeError(f"BLE device {args.name!r} not found")

        inbox = Inbox(verbose=args.verbose)

        async with BleakClient(
            device,
            timeout=args.connect_timeout,
        ) as client:
            log(f"adelie: connected to {device.name or device.address}")

            print_services(client)

            log(f"adelie: enabling notifications on {TX_UUID}")

            await asyncio.wait_for(
                client.start_notify(
                    TX_UUID,
                    inbox.callback,
                ),
                timeout=args.timeout,
            )

            log("adelie: notifications enabled")

            protocol_lock = asyncio.Lock()

            tracker = RollingClockTracker(args.model_window)

            clock_task = asyncio.create_task(
                clock_sync_worker(
                    client=client,
                    inbox=inbox,
                    tracker=tracker,
                    clock_writer=clock_writer,
                    protocol_lock=protocol_lock,
                    timeout=args.timeout,
                    sync_interval=args.sync_interval,
                    stop_event=clock_stop,
                    verbose=args.verbose,
                ),
                name="adelie-clock-sync",
            )

            acquisition_timeout = (
                args.model_window * args.sync_interval + args.timeout + 5.0
            )

            log(
                "adelie: acquiring rolling "
                "clock model "
                f"window={args.model_window} "
                f"interval="
                f"{args.sync_interval:.3f} s"
            )

            await asyncio.wait_for(
                tracker.ready.wait(),
                timeout=acquisition_timeout,
            )

            model, generation = await tracker.snapshot()

            log(
                "adelie: clock model ready "
                f"generation={generation} "
                f"points={model.sample_count} "
                f"min network RTT="
                f"{model.minimum_network_rtt_us:.1f} us "
                f"median="
                f"{model.median_network_rtt_us:.1f} us "
                f"slope error="
                f"{model.slope_error_ppm:.3f} ppm"
            )

            for index in range(args.commands):
                sequence = 10_000 + index

                log(f"adelie: command {index + 1}/{args.commands} sequence={sequence}")

                async with protocol_lock:
                    model, generation = await tracker.snapshot()

                    row = await run_command(
                        client=client,
                        inbox=inbox,
                        model=model,
                        model_generation=generation,
                        sequence=sequence,
                        timeout=args.timeout,
                    )

                command_writer.append(row)

                log(
                    "adelie: command "
                    f"sequence={sequence} "
                    f"RTT="
                    f"{float(row['total_rtt_us']):.1f} us "
                    f"model_generation="
                    f"{generation}"
                )

                if index + 1 < args.commands:
                    await asyncio.sleep(args.command_interval)

            if args.track_after_commands > 0:
                log(
                    "adelie: continuing clock "
                    "tracking for "
                    f"{args.track_after_commands:.1f} s"
                )

                await asyncio.sleep(args.track_after_commands)

            clock_stop.set()

            if clock_task is not None:
                await clock_task
                clock_task = None

            await client.stop_notify(TX_UUID)

            log("adelie: notifications stopped")

        log("adelie: completed successfully")

        return 0

    except Exception as error:
        log(f"adelie: FAILED: {type(error).__name__}: {error}")

        traceback.print_exc()

        log(
            "adelie: partial output retained: "
            f"{clock_writer.path} and "
            f"{command_writer.path}"
        )

        return 1

    finally:
        clock_stop.set()

        if clock_task is not None:
            clock_task.cancel()

            try:
                await clock_task

            except asyncio.CancelledError:
                pass

            except Exception:
                if args.verbose:
                    traceback.print_exc()

        clock_writer.close()
        command_writer.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Maintain a rolling "
            "Adelie-to-Korora clock model "
            "and measure reward_port "
            "command latency."
        )
    )

    parser.add_argument(
        "--name",
        default=DEVICE_NAME,
    )

    parser.add_argument(
        "--scan-timeout",
        type=float,
        default=15.0,
    )

    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=20.0,
    )

    parser.add_argument(
        "--timeout",
        type=float,
        default=3.0,
    )

    parser.add_argument(
        "--model-window",
        type=int,
        default=MODEL_WINDOW_SIZE,
        help=("rolling affine-model window size"),
    )

    parser.add_argument(
        "--sync-interval",
        type=float,
        default=DEFAULT_SYNC_INTERVAL_S,
        help=("seconds between continuous clock exchanges"),
    )

    parser.add_argument(
        "--commands",
        type=int,
        default=1,
    )

    parser.add_argument(
        "--command-interval",
        type=float,
        default=0.5,
    )

    parser.add_argument(
        "--track-after-commands",
        type=float,
        default=0.0,
        help=(
            "continue updating the clock "
            "model for this many seconds "
            "after the final command"
        ),
    )

    parser.add_argument(
        "--clock-csv",
        type=Path,
        default=Path("adelie_clock_samples.csv"),
    )

    parser.add_argument(
        "--csv",
        type=Path,
        default=Path("adelie_latency.csv"),
    )

    parser.add_argument(
        "--verbose",
        action="store_true",
    )

    return parser.parse_args()


def main() -> None:
    sys.exit(asyncio.run(async_main(parse_args())))


if __name__ == "__main__":
    main()
