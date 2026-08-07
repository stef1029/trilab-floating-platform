# Adelie Python application

Adelie is the universal host application for board setup, live control,
recording, static analysis, and plotting. It is written for Python 3.14 and
also supports Python 3.11 or newer.

## Features

- Bleak connection to Korora
- Modern PySide6 interface
- One live card per Korora, Galapagos, and Fairy
- Live RMS history, skew, record rate, queue, loss, and RSSI
- Exact UUID configuration saved as JSON
- White LED board identification
- RGB, IR, tone, white noise, and valve controls
- Telemetry level selection
- Scheduled TTL train control
- One button sync test
- One locked JSON Lines log writer
- Static parser, analyser, and plotter

## Install

```bash
python -m venv .venv
python -m pip install --upgrade pip
python -m pip install -e .
```

Run:

```bash
adelie
```

Optional paths:

```bash
adelie --config config/boards.json --recordings recordings
```

## Board configuration

Adelie requests the full live inventory after BLE connects.

When the saved UUID set matches exactly, Adelie sends the saved Fairy indices
to Korora. When it differs, the table remains in configuration mode.

To create a configuration:

1. Click White LED on one row
2. Find the board showing white
3. Enter its zero based Fairy index and label
4. Repeat for every row
5. Save the exact UUID configuration

The JSON file is the backup. Korora stores no assignment across a power cycle.

If any live UUID is added, missing, or replaced, automatic assignment stops.

## Recording

Choose the `.log` path before Record Start. The recorder creates a new file
and refuses to overwrite an existing file.

Each line is one JSON value. The file contains:

- Header with protocol versions and UUID mapping
- Every Fairy record received during the session
- Raw application message in Base64
- Host monotonic receive time
- Footer with stop reason

Nothing is written before Record Start. An unexpected BLE disconnect closes
the file with a footer when the process still has control.

Record Stop keeps the writer open briefly after the stop response so final
output off records can arrive. A Fairy inventory change or a critical hardware
fault requests the same orderly stop.

## Telemetry

Critical mode keeps experiment events and faults.

Standard mode also keeps health, inventory, link, and clock quality.

Full mode also keeps every raw SYNC observation and clock pair. Test sync
selects full mode, sends safe random RGB commands, and starts TTL at 1 Hz.

## Static tools

Run the full pipeline:

```bash
adelie-run-analysis recordings/run_20260730_120000.log
```

Or run each stage:

```bash
adelie-parse RUN.log
adelie-analyse RUN_parsed
adelie-plot RUN_parsed
```

The parser creates:

- `records.csv`
- `commands.csv`
- `command_results.csv`
- `sync_pairs.csv`
- `sync_quality.csv`
- `ttl.csv`
- `events.csv`
- `health.csv`
- `manifest.json`

The analyser creates:

- `node_summary.csv`
- `command_summary.csv`
- `prediction_errors.csv`
- `ttl_results.csv`
- `summary.json`
- `summary.txt`

The plotter creates synchronization, per node prediction error, command RTT,
and TTL alignment PNG files when the required records exist.

Every parsed record includes its numeric logical address, logical node name,
saved label, and Fairy index. For example, address `0x10` is reported as
`fairy0` rather than a generic node number.

`commands.csv` contains one row per Adelie command with:

- Time waiting in the host transmit queue
- Queue to response RTT
- Actual BLE send to response RTT
- Destination logical node
- Opcode and response status
- Missing response detection
- Matching command result count and result nodes
- Clock exchange full RTT, Korora processing time, and network RTT

`command_results.csv` matches Fairy command result records back to the Adelie
command ID and reports them separately for every logical Fairy.

Command timing fields are available only in logs recorded with this version of
Adelie. Older logs did not store outbound command or response timestamps, so
their command RTT cannot be reconstructed.

## Log recovery

Every record is one complete flushed line. If the host loses power, valid
lines before the partial final line remain usable. The parser reports the
line number of malformed JSON so a partial last line can be removed without
changing earlier data.
