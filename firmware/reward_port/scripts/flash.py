#!/usr/bin/env python3
"""Flash one node board, giving it its node number.

    flash.py --id 3     build, flash firmware, and set this board to node 3
    flash.py            build and flash firmware, keeping the board's number
    flash.py --read     report the board's number and UUID, change nothing

The number (0 to RIG_MAX_NODES - 1) is the board's identity: its RS485
address is 0x10 + number. It lives in the last flash page, outside the
firmware image, so firmware-only flashes (this script without --id,
flash_all.py, or PlatformIO upload) never change it. The record format is
defined in firmware/shared/include/rig_protocol/node_identity.hpp.

Connect exactly one ST-Link, or pick one with --serial.
"""

from __future__ import annotations

import argparse
import binascii
import re
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from flash_all import (
    Probe,
    discover_probes,
    find_openocd,
    find_program,
    openocd_braced,
    print_probe,
    run,
)

DEFAULT_ENVIRONMENT = "final_pcb_g071gb"

# Must match rig_protocol/node_identity.hpp.
IDENTITY_PAGE_ADDRESS = 0x0801F800
IDENTITY_PAGE_SIZE = 2048
IDENTITY_MAGIC = 0x44494E52
IDENTITY_VERSION = 1
IDENTITY_RECORD_SIZE = 16
UID_ADDRESS = 0x1FFF7590
UID_SIZE = 12

SYSTEM_CONFIG = (
    Path(__file__).resolve().parents[2]
    / "shared" / "include" / "rig_protocol" / "system_config.hpp"
)


def max_nodes() -> int:
    """Read the RIG_MAX_NODES default the node firmware is built with."""
    text = SYSTEM_CONFIG.read_text(encoding="utf-8")
    match = re.search(r"#define\s+RIG_MAX_NODES\s+(\d+)", text)
    if not match:
        raise RuntimeError(f"RIG_MAX_NODES not found in {SYSTEM_CONFIG}")
    return int(match.group(1))


def encode_identity(node_id: int) -> bytes:
    body = struct.pack(
        "<IBBB7x", IDENTITY_MAGIC, IDENTITY_VERSION, node_id, (~node_id) & 0xFF
    )
    return body + struct.pack("<H", binascii.crc_hqx(body, 0xFFFF))


def decode_identity(record: bytes, limit: int) -> int | None:
    if len(record) < IDENTITY_RECORD_SIZE:
        return None
    record = record[:IDENTITY_RECORD_SIZE]
    magic, version, node_id, inverse = struct.unpack_from("<IBBB", record)
    (crc,) = struct.unpack_from("<H", record, IDENTITY_RECORD_SIZE - 2)
    if (
        magic != IDENTITY_MAGIC
        or version != IDENTITY_VERSION
        or inverse != (~node_id) & 0xFF
        or crc != binascii.crc_hqx(record[:-2], 0xFFFF)
        or node_id >= limit
    ):
        return None
    return node_id


@dataclass(frozen=True)
class BoardState:
    node_id: int | None
    blank: bool
    uuid: str


def openocd_base(openocd: str, probe: Probe) -> list[str]:
    return [
        openocd,
        "-f", "interface/stlink.cfg",
        "-c", f"adapter serial {probe.serial}",
        "-c", "gdb_port disabled",
        "-c", "tcl_port disabled",
        "-c", "telnet_port disabled",
        "-f", "target/stm32g0x.cfg",
    ]


def run_openocd(command: list[str], verbose: bool, what: str) -> None:
    result = run(command)
    if verbose or result.returncode != 0:
        print(result.stdout.rstrip())
    if result.returncode != 0:
        raise RuntimeError(f"OpenOCD failed while {what}")


def read_board(openocd: str, probe: Probe, limit: int,
               verbose: bool) -> BoardState:
    with tempfile.TemporaryDirectory() as folder:
        identity_file = Path(folder) / "identity.bin"
        uid_file = Path(folder) / "uid.bin"
        command = openocd_base(openocd, probe) + [
            "-c", "init",
            "-c", "reset halt",
            "-c", f"dump_image {openocd_braced(identity_file)} "
                  f"{IDENTITY_PAGE_ADDRESS:#x} {IDENTITY_RECORD_SIZE}",
            "-c", f"dump_image {openocd_braced(uid_file)} "
                  f"{UID_ADDRESS:#x} {UID_SIZE}",
            "-c", "reset run",
            "-c", "shutdown",
        ]
        run_openocd(command, verbose, "reading the board")
        record = identity_file.read_bytes()
        uid = uid_file.read_bytes()
    # The firmware packs the UID words little endian, so the raw bytes are
    # already the UUID that Adelie and the hub log.
    return BoardState(
        node_id=decode_identity(record, limit),
        blank=record == b"\xff" * len(record),
        uuid=uid.hex(),
    )


def flash_firmware(openocd: str, probe: Probe, firmware: Path,
                   verbose: bool) -> None:
    command = openocd_base(openocd, probe) + [
        "-c", f"program {openocd_braced(firmware)} verify reset exit",
    ]
    run_openocd(command, verbose, "flashing firmware")


def flash_firmware_and_identity(openocd: str, probe: Probe, firmware: Path,
                                node_id: int, verbose: bool) -> None:
    with tempfile.TemporaryDirectory() as folder:
        identity_file = Path(folder) / "identity.bin"
        identity_file.write_bytes(encode_identity(node_id))
        identity = openocd_braced(identity_file)
        command = openocd_base(openocd, probe) + [
            "-c", f"program {openocd_braced(firmware)} verify",
            "-c", f"flash erase_address {IDENTITY_PAGE_ADDRESS:#x} "
                  f"{IDENTITY_PAGE_SIZE}",
            "-c", f"flash write_image {identity} {IDENTITY_PAGE_ADDRESS:#x} bin",
            "-c", f"verify_image {identity} {IDENTITY_PAGE_ADDRESS:#x} bin",
            "-c", "reset run",
            "-c", "shutdown",
        ]
        run_openocd(command, verbose, "flashing firmware and node number")


def describe(state: BoardState) -> str:
    if state.node_id is not None:
        return f"node {state.node_id} (address {0x10 + state.node_id:#04x})"
    return "no node number" if state.blank else "corrupt node number"


def parse_arguments(limit: int) -> argparse.Namespace:
    project_dir = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Flash one node board and give it its node number.",
    )
    action = parser.add_mutually_exclusive_group()
    action.add_argument(
        "--id", type=int, metavar="N",
        help=f"set this board's node number (0 to {limit - 1})",
    )
    action.add_argument(
        "--read", action="store_true",
        help="only report the board's node number and UUID",
    )
    parser.add_argument("--serial", help="ST-Link serial, if several are connected")
    parser.add_argument("-e", "--environment", default=DEFAULT_ENVIRONMENT,
                        help=f"PlatformIO environment (default: {DEFAULT_ENVIRONMENT})")
    parser.add_argument("--project-dir", type=Path, default=project_dir,
                        help=f"PlatformIO project directory (default: {project_dir})")
    parser.add_argument("--no-build", action="store_true",
                        help="flash the existing firmware ELF without building")
    parser.add_argument("--pio", help="path to the PlatformIO pio executable")
    parser.add_argument("--openocd", help="path to the OpenOCD executable")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="print OpenOCD and build output")
    args = parser.parse_args()
    if args.id is not None and not 0 <= args.id < limit:
        parser.error(f"--id must be between 0 and {limit - 1}")
    return args


def select_probe(args: argparse.Namespace, pio: str | None) -> Probe:
    if args.serial:
        return Probe(serial=args.serial, source="command line")
    probes = discover_probes(pio)
    if not probes:
        raise RuntimeError("no connected ST-Link probes were found")
    if len(probes) > 1:
        for probe in probes:
            print_probe(probe)
        raise RuntimeError(
            f"{len(probes)} ST-Link probes are connected; flash one board at "
            "a time, or choose one with --serial"
        )
    return probes[0]


def build(pio: str | None, args: argparse.Namespace) -> Path:
    project_dir = args.project_dir.expanduser().resolve()
    if not args.no_build:
        if pio is None:
            raise RuntimeError("PlatformIO is required to build the firmware")
        print(f"Building {args.environment}...")
        result = run((pio, "run", "--project-dir", str(project_dir),
                      "--environment", args.environment))
        if args.verbose or result.returncode != 0:
            print(result.stdout.rstrip())
        if result.returncode != 0:
            raise RuntimeError("PlatformIO build failed")
    firmware = project_dir / ".pio" / "build" / args.environment / "firmware.elf"
    if not firmware.is_file():
        raise RuntimeError(f"firmware image does not exist: {firmware}")
    return firmware


def main() -> int:
    limit = max_nodes()
    args = parse_arguments(limit)

    pio: str | None = None
    try:
        pio = find_program(args.pio, ("pio", "platformio"))
    except FileNotFoundError:
        pass

    try:
        openocd = find_openocd(args.openocd)
        probe = select_probe(args, pio)
        print(f"ST-Link {probe.serial}")

        before = read_board(openocd, probe, limit, args.verbose)
        print(f"Board UUID {before.uuid}: {describe(before)}")
        if args.read:
            return 0 if before.node_id is not None else 1

        if args.id is None and before.node_id is None:
            raise RuntimeError(
                f"this board has {describe(before)}; flash it with "
                f"--id N (0 to {limit - 1}) to give it one"
            )

        firmware = build(pio, args)

        if args.id is None:
            print(f"Flashing firmware, keeping node {before.node_id}...")
            flash_firmware(openocd, probe, firmware, args.verbose)
            after = read_board(openocd, probe, limit, args.verbose)
            if after.node_id != before.node_id:
                raise RuntimeError(
                    f"node number changed during flashing: now {describe(after)}"
                )
        else:
            if before.node_id is not None and before.node_id != args.id:
                print(f"Changing node {before.node_id} to node {args.id}.")
            print(f"Flashing firmware and node number {args.id}...")
            flash_firmware_and_identity(openocd, probe, firmware, args.id,
                                        args.verbose)
            after = BoardState(node_id=args.id, blank=False, uuid=before.uuid)
    except (FileNotFoundError, RuntimeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"Done: board {after.uuid} is {describe(after)}. "
          f"Label it {after.node_id}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
