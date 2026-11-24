# Adelie Python application

Adelie is the universal host application for board setup, live control,
recording, static analysis, and plotting. It is written for Python 3.14 and
also supports Python 3.11 or newer.

## Two modes

The window has a **Simple / Developer** switch in the top bar. Connect and
recording controls are in the top bar in both modes. The app reopens in
whichever mode was used last; `adelie --simple` or `adelie --dev` forces one.

**Simple** is for running sessions:

- A ready-to-record banner that says what it is waiting for
- Health tiles for the hub link, battery, ports, sync box and comms error rate
- One tile per configured port: status, beam breaks, reward (valve pulse),
  light colour presets with brightness, and tone / noise playback

**Developer** has every diagnostic and raw control, in tabs:

- *Boards*: one live card per Hub, TTL generator and node, with RGB / IR /
  audio / valve controls
- *Link & clock*: BLE link, RS485 gateway counters, host clock exchange
- *TTL & sync*: telemetry level, sync test, TTL train and loop timing
- *Hub sensors & power*: IMU, magnetometer, live pose, nPM1300, status LEDs
- *Board setup*: the UUID to node index table (see below)

Code layout: `adelie/controller.py` talks to the hardware, `adelie/health.py`
turns its state into the health summary (no Qt, unit tested), and
`adelie/gui/` holds the window (`main_window.py`, `session_bar.py`),
`gui/simple/` and `gui/dev/` (one module per panel).

## Features

- Bleak connection to the hub
- Modern PySide6 interface
- One live card per Hub, TTL generator, and node
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

Optional paths and start mode:

```bash
adelie --config config/boards.json --recordings recordings
adelie --dev        # or --simple
```

Run the tests:

```bash
python -m pip install -e ".[dev]"
python -m pytest
```

## Board configuration

Board setup lives in Developer › Board setup. Simple mode shows
"Board setup needed" when the live boards do not match the saved set.

Adelie requests the full live inventory after BLE connects.

When the saved UUID set matches exactly, Adelie sends the saved node indices
to the hub. When it differs, the table remains in configuration mode.

To create a configuration:

1. Click White LED on one row
2. Find the board showing white
3. Enter its zero based node index and label
4. Repeat for every row
5. Save the exact UUID configuration

The JSON file is the backup. Hub stores no assignment across a power cycle.

If any live UUID is added, missing, or replaced, automatic assignment stops.

## Recording

Choose the `.log` path before Record Start. The recorder creates a new file
and refuses to overwrite an existing file.

Each line is one JSON value. The file contains:

- Header with protocol versions and UUID mapping
- Every record received during the session
- Raw application message in Base64
- Host monotonic receive time
- Footer with stop reason

Nothing is written before Record Start. An unexpected BLE disconnect closes
the file with a footer when the process still has control.

Record Stop keeps the writer open briefly after the stop response so final
output off records can arrive. A node inventory change or a critical hardware
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
saved label, and node index. For example, address `0x10` is reported as
`node0` rather than a generic node number.

`commands.csv` contains one row per Adelie command with:

- Time waiting in the host transmit queue
- Queue to response RTT
- Actual BLE send to response RTT
- Destination logical node
- Opcode and response status
- Missing response detection
- Matching command result count and result nodes
- Clock exchange full RTT, Hub processing time, and network RTT

`command_results.csv` matches node command result records back to the Adelie
command ID and reports them separately for every logical node.

Command timing fields are available only in logs recorded with this version of
Adelie. Older logs did not store outbound command or response timestamps, so
their command RTT cannot be reconstructed.

## Log recovery

Every record is one complete flushed line. If the host loses power, valid
lines before the partial final line remain usable. The parser reports the
line number of malformed JSON so a partial last line can be removed without
changing earlier data.
