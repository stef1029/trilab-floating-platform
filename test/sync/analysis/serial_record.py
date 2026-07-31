from __future__ import annotations

import argparse
import os
import signal
import sys
import time
from pathlib import Path

try:
    import serial
    from serial import SerialException
except ImportError as exc:
    raise SystemExit(
        "pyserial is required:\n  python3 -m pip install pyserial"
    ) from exc


_STOP = False


def handle_stop(signum: int, frame: object) -> None:
    del signum, frame
    global _STOP
    _STOP = True


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Record Korora serial output using explicit 8N1."
    )
    parser.add_argument(
        "port",
        help="Serial device, for example /dev/ttyACM1",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        required=True,
        help="Output .log file",
    )
    parser.add_argument(
        "-b",
        "--baud",
        type=int,
        default=115200,
        help="Baud rate; default: 115200",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=None,
        help="Optional recording duration in seconds",
    )
    parser.add_argument(
        "--append",
        action="store_true",
        help="Append instead of refusing an existing file",
    )
    parser.add_argument(
        "--no-echo",
        action="store_true",
        help="Do not print received serial data to the terminal",
    )
    parser.add_argument(
        "--retry-seconds",
        type=float,
        default=2.0,
        help="Delay before retrying after an open/read failure",
    )
    parser.add_argument(
        "--no-reconnect",
        action="store_true",
        help="Exit after the first disconnect or open failure",
    )
    return parser.parse_args()


def open_korora_serial(port: str, baud: int) -> serial.Serial:
    device = serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.25,
        write_timeout=1.0,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
        exclusive=True,
    )

    # Zephyr USB CDC ACM commonly uses DTR as the indication that a terminal
    # is connected. Make the line state explicit rather than relying on the
    # backend default.
    device.dtr = True
    device.rts = False

    return device


def main() -> None:
    args = parse_args()

    if args.baud <= 0:
        raise SystemExit("--baud must be positive")
    if args.duration is not None and args.duration <= 0:
        raise SystemExit("--duration must be positive")
    if args.retry_seconds < 0:
        raise SystemExit("--retry-seconds cannot be negative")

    output_path = args.output.expanduser()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    if output_path.exists() and not args.append:
        raise SystemExit(
            f"Output already exists: {output_path}\n"
            "Use another filename or pass --append."
        )

    mode = "ab" if args.append else "xb"

    signal.signal(signal.SIGINT, handle_stop)
    signal.signal(signal.SIGTERM, handle_stop)

    started = time.monotonic()
    deadline = started + args.duration if args.duration is not None else None

    total_bytes = 0

    print(f"Port   : {args.port}")
    print(f"Format : {args.baud} baud, 8N1")
    print(f"Output : {output_path}")
    print("Stop   : Ctrl-C")
    print()

    with output_path.open(mode, buffering=0) as output:
        while not _STOP:
            if deadline is not None and time.monotonic() >= deadline:
                break

            try:
                device = open_korora_serial(args.port, args.baud)
            except TypeError:
                # Some pyserial backends do not implement exclusive=.
                try:
                    device = serial.Serial(
                        port=args.port,
                        baudrate=args.baud,
                        bytesize=serial.EIGHTBITS,
                        parity=serial.PARITY_NONE,
                        stopbits=serial.STOPBITS_ONE,
                        timeout=0.25,
                        write_timeout=1.0,
                        xonxoff=False,
                        rtscts=False,
                        dsrdtr=False,
                    )
                except (SerialException, OSError) as exc:
                    print(
                        f"[serial] open failed: {exc}",
                        file=sys.stderr,
                        flush=True,
                    )
                    if args.no_reconnect:
                        raise SystemExit(1) from exc
                    time.sleep(args.retry_seconds)
                    continue
            except (SerialException, OSError) as exc:
                print(
                    f"[serial] open failed: {exc}",
                    file=sys.stderr,
                    flush=True,
                )
                if args.no_reconnect:
                    raise SystemExit(1) from exc
                time.sleep(args.retry_seconds)
                continue

            print(
                "[serial] connected as 8N1",
                file=sys.stderr,
                flush=True,
            )

            try:
                while not _STOP:
                    if deadline is not None and time.monotonic() >= deadline:
                        break

                    try:
                        data = device.read(4096)
                    except (SerialException, OSError) as exc:
                        print(
                            f"\n[serial] read failed: {exc}",
                            file=sys.stderr,
                            flush=True,
                        )
                        break

                    if not data:
                        continue

                    output.write(data)
                    total_bytes += len(data)

                    if not args.no_echo:
                        sys.stdout.write(data.decode("utf-8", errors="replace"))
                        sys.stdout.flush()

            finally:
                try:
                    device.close()
                except Exception:
                    pass

                output.flush()
                os.fsync(output.fileno())

            if (
                _STOP
                or args.no_reconnect
                or (deadline is not None and time.monotonic() >= deadline)
            ):
                break

            print(
                f"[serial] reconnecting in {args.retry_seconds:g}s",
                file=sys.stderr,
                flush=True,
            )
            time.sleep(args.retry_seconds)

    elapsed = time.monotonic() - started
    print()
    print(f"Recorded {total_bytes} bytes in {elapsed:.2f} s")
    print(f"Saved to {output_path}")


if __name__ == "__main__":
    main()
