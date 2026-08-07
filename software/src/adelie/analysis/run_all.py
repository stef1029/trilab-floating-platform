from __future__ import annotations

import argparse
from pathlib import Path

from .analyse import analyse
from .parse_log import parse
from .plot import create_plots


def run(log_path: Path, output_directory: Path) -> None:
    parse(log_path, output_directory)
    analyse(output_directory)
    create_plots(output_directory)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Parse, analyse, and plot an Adelie log"
    )
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output = args.output or args.log.with_name(f"{args.log.stem}_analysis")
    run(args.log, output)
    print(output)


if __name__ == "__main__":
    main()
