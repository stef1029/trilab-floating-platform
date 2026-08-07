#!/usr/bin/env python3
"""Build Fairy once and flash every connected ST-Link probe."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


DEFAULT_ENVIRONMENT = "nucleo_g071rb_auto_rs485"
ST_VENDOR_ID = "0483"
STLINK_PRODUCT_IDS = {
    "3744",
    "3748",
    "374a",
    "374b",
    "374d",
    "374e",
    "374f",
    "3752",
    "3753",
    "3754",
}
SERIAL_PATTERN = re.compile(r"(?:^|\s)SER=([^\s]+)", re.IGNORECASE)


@dataclass(frozen=True)
class Probe:
    serial: str
    port: str = ""
    source: str = ""


@dataclass(frozen=True)
class FlashResult:
    probe: Probe
    returncode: int
    output: str


def run(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    check: bool = False,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(command),
        cwd=cwd,
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def find_program(explicit: str | None, names: Iterable[str]) -> str:
    if explicit:
        path = shutil.which(explicit) or str(Path(explicit).expanduser())
        if Path(path).is_file() or shutil.which(path):
            return path
        raise FileNotFoundError(f"program not found: {explicit}")

    for name in names:
        path = shutil.which(name)
        if path:
            return path
    raise FileNotFoundError(
        f"none of these programs were found: {', '.join(names)}")


def find_openocd(explicit: str | None) -> str:
    if explicit:
        return find_program(explicit, ())

    path = shutil.which("openocd")
    if path:
        return path

    core_dir = Path(
        os.environ.get("PLATFORMIO_CORE_DIR", str(Path.home() / ".platformio"))
    ).expanduser()
    executable = "openocd.exe" if os.name == "nt" else "openocd"
    candidates = (
        core_dir / "packages" / "tool-openocd" / "bin" / executable,
        core_dir / "packages" / "tool-openocd-esp32" / "bin" / executable,
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)

    raise FileNotFoundError(
        "OpenOCD was not found in PATH or the PlatformIO package directory"
    )


def discover_with_platformio(pio: str) -> list[Probe]:
    result = run((pio, "device", "list", "--json-output"))
    if result.returncode != 0:
        return []

    try:
        devices = json.loads(result.stdout)
    except json.JSONDecodeError:
        return []

    probes: list[Probe] = []
    for device in devices if isinstance(devices, list) else []:
        if not isinstance(device, dict):
            continue
        description = str(device.get("description", ""))
        hwid = str(device.get("hwid", ""))
        combined = f"{description} {hwid}".lower()
        match = SERIAL_PATTERN.search(hwid)
        if (
            match
            and f"vid:pid={ST_VENDOR_ID}:" in combined
            and ("st-link" in combined or "stlink" in combined)
        ):
            probes.append(
                Probe(
                    serial=match.group(1),
                    port=str(device.get("port", "")),
                    source="PlatformIO",
                )
            )
    return probes


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="ascii").strip()
    except (OSError, UnicodeError):
        return ""


def discover_with_linux_sysfs() -> list[Probe]:
    usb_root = Path("/sys/bus/usb/devices")
    if not usb_root.is_dir():
        return []

    probes: list[Probe] = []
    for device in usb_root.iterdir():
        vendor = read_text(device / "idVendor").lower()
        product_id = read_text(device / "idProduct").lower()
        product_name = read_text(device / "product").lower()
        serial = read_text(device / "serial")
        is_stlink = "st-link" in product_name or "stlink" in product_name
        if (
            vendor == ST_VENDOR_ID
            and serial
            and (is_stlink or product_id in STLINK_PRODUCT_IDS)
        ):
            probes.append(Probe(serial=serial, source="Linux USB"))
    return probes


def merge_probes(probes: Iterable[Probe]) -> list[Probe]:
    by_serial: dict[str, Probe] = {}
    for probe in probes:
        key = probe.serial.casefold()
        previous = by_serial.get(key)
        if previous is None or (not previous.port and probe.port):
            by_serial[key] = probe
    return sorted(by_serial.values(), key=lambda probe: probe.serial.casefold())


def discover_probes(pio: str | None) -> list[Probe]:
    platformio_probes = discover_with_platformio(pio) if pio else []
    return merge_probes((*platformio_probes, *discover_with_linux_sysfs()))


def openocd_braced(value: str | Path) -> str:
    normalized = str(value).replace("\\", "/")
    return "{" + normalized.replace("}", "\\}") + "}"


def flash_one(openocd: str, firmware: Path, probe: Probe) -> FlashResult:
    command = (
        openocd,
        "-f",
        "interface/stlink.cfg",
        "-c",
        f"adapter serial {probe.serial}",
        "-c",
        "gdb_port disabled",
        "-c",
        "tcl_port disabled",
        "-c",
        "telnet_port disabled",
        "-f",
        "target/stm32g0x.cfg",
        "-c",
        f"program {openocd_braced(firmware)} verify reset exit",
    )
    result = run(command)
    return FlashResult(probe, result.returncode, result.stdout)


def print_probe(probe: Probe) -> None:
    port = f"  {probe.port}" if probe.port else ""
    source = f"  [{probe.source}]" if probe.source else ""
    print(f"{probe.serial}{port}{source}")


def parse_arguments() -> argparse.Namespace:
    script_project_dir = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description=(
            "Discover all connected ST-Link probes, build Fairy once, and flash "
            "the same verified image to every board."
        )
    )
    parser.add_argument(
        "--project-dir",
        type=Path,
        default=script_project_dir,
        help=f"PlatformIO project directory (default: {script_project_dir})",
    )
    parser.add_argument(
        "-e",
        "--environment",
        default=DEFAULT_ENVIRONMENT,
        help=f"PlatformIO environment (default: {DEFAULT_ENVIRONMENT})",
    )
    parser.add_argument(
        "--serial",
        action="append",
        default=[],
        help="flash this ST-Link serial instead of discovery; may be repeated",
    )
    parser.add_argument(
        "--firmware",
        type=Path,
        help="firmware ELF to flash (default: PlatformIO build output)",
    )
    parser.add_argument(
        "--no-build",
        action="store_true",
        help="use the existing firmware ELF without building",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="list detected probes without building or flashing",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=0,
        help="maximum simultaneous flashes (default: number of boards)",
    )
    parser.add_argument("--pio", help="path to the PlatformIO pio executable")
    parser.add_argument("--openocd", help="path to the OpenOCD executable")
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="print OpenOCD output for successful flashes too",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    project_dir = args.project_dir.expanduser().resolve()

    pio: str | None = None
    try:
        pio = find_program(args.pio, ("pio", "platformio"))
    except FileNotFoundError as error:
        if not args.no_build and not args.list:
            print(f"ERROR: {error}", file=sys.stderr)
            return 2

    probes = (
        merge_probes(Probe(serial=value, source="command line")
                     for value in args.serial)
        if args.serial
        else discover_probes(pio)
    )
    if not probes:
        print("ERROR: no connected ST-Link probes were found", file=sys.stderr)
        return 3

    print(f"Found {len(probes)} ST-Link probe(s):")
    for probe in probes:
        print_probe(probe)

    if args.list:
        return 0

    if not args.no_build:
        if pio is None:
            print("ERROR: PlatformIO is required to build the firmware",
                  file=sys.stderr)
            return 2
        print(f"Building {args.environment} once...")
        build = run(
            (
                pio,
                "run",
                "--project-dir",
                str(project_dir),
                "--environment",
                args.environment,
            )
        )
        if build.returncode != 0:
            print(build.stdout, end="")
            print("ERROR: PlatformIO build failed", file=sys.stderr)
            return build.returncode or 4

    firmware = (
        args.firmware.expanduser().resolve()
        if args.firmware
        else project_dir / ".pio" / "build" / args.environment / "firmware.elf"
    )
    if not firmware.is_file():
        print(
            f"ERROR: firmware image does not exist: {firmware}", file=sys.stderr)
        return 5

    try:
        openocd = find_openocd(args.openocd)
    except FileNotFoundError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2

    jobs = len(probes) if args.jobs <= 0 else min(args.jobs, len(probes))
    print(f"Flashing {len(probes)} board(s) with {jobs} worker(s)...")
    results: list[FlashResult] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as executor:
        futures = {
            executor.submit(flash_one, openocd, firmware, probe): probe
            for probe in probes
        }
        for future in concurrent.futures.as_completed(futures):
            probe = futures[future]
            try:
                result = future.result()
            except OSError as error:
                result = FlashResult(probe, 127, str(error))
            results.append(result)
            state = "OK" if result.returncode == 0 else "FAILED"
            print(f"[{state}] {probe.serial}")
            if args.verbose or result.returncode != 0:
                print(result.output.rstrip())

    failures = [result for result in results if result.returncode != 0]
    if failures:
        print(
            f"ERROR: {len(failures)} of {len(results)} board(s) failed",
            file=sys.stderr,
        )
        return 1

    print(f"Successfully flashed and verified all {len(results)} board(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
