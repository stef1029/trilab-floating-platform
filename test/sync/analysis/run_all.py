"""Run parse -> analyse -> plot with one command."""

from __future__ import annotations

import argparse
from pathlib import Path

from analyse_sync import analyse_directory
from parse_sync_log import parse_inputs
from plot_sync import generate_plots


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Parse and analyse Korora serial and Adelie CSV data."
    )
    parser.add_argument("--korora", type=Path, help="Korora raw serial log")
    parser.add_argument(
        "--adelie",
        nargs="+",
        type=Path,
        help=("Adelie output directory, one Adelie CSV, or the clock and latency CSVs"),
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=Path("sync_results"),
    )
    parser.add_argument("--hub-hz", type=float, default=16_000_000.0)
    parser.add_argument("--local-hz", type=float, default=16_000_000.0)
    parser.add_argument("--window", type=int, default=16)
    parser.add_argument(
        "--node",
        default="all",
        help="all or comma-separated nodes, e.g. fairy,galapagos,adelie",
    )
    parser.add_argument(
        "--profile",
        choices=["curated", "expanded", "full"],
        default="expanded",
    )
    parser.add_argument("--no-plots", action="store_true")
    parser.add_argument("--show", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    if args.korora is None and not args.adelie:
        raise SystemExit("Provide at least one of --korora or --adelie")
    if args.window < 3:
        raise SystemExit("--window must be at least 3")

    parsed = args.output_root / "parsed"
    analysis = args.output_root / "analysis"
    plots = args.output_root / "plots"

    manifest = parse_inputs(
        korora=args.korora,
        adelie=args.adelie,
        output=parsed,
    )

    summary = analyse_directory(
        input_dir=parsed,
        output_dir=analysis,
        hub_hz=args.hub_hz,
        local_hz=args.local_hz,
        window_size=args.window,
    )

    created = []
    if not args.no_plots:
        created = generate_plots(
            parsed_dir=parsed,
            analysis_dir=analysis,
            output_dir=plots,
            node_selection=args.node,
            profile=args.profile,
            show=args.show,
        )

    print()
    print(f"Parsed directory:  {parsed}")
    print(f"Analysis directory:{analysis}")
    if not args.no_plots:
        print(f"Plot directory:    {plots}")
        print(f"Plot count:        {len(created)}")
    print(f"Summary rows:      {len(summary)}")
    print(f"Korora events:     {manifest['counts']['events']}")
    print(f"Adelie samples:    {manifest['counts']['adelie_clock']}")
    print(f"Adelie commands:   {manifest['counts']['adelie_commands']}")


if __name__ == "__main__":
    main()
