# Adelie command protocol v2

Command v2 carries commands from the Python application and responses from the target board. Commands use a fixed header and flexible TLV fields.

## Message header

| Offset | Size | Field            | Meaning                            |
| -----: | ---: | ---------------- | ---------------------------------- |
|      0 |    2 | magic            | `0xAD1E`                           |
|      2 |    1 | version          | `2`                                |
|      3 |    1 | header size      | `32`                               |
|      4 |    1 | kind             | Command or response                |
|      5 |    1 | status           | Response status                    |
|      6 |    2 | opcode           | Requested operation                |
|      8 |    2 | flags            | Command behavior                   |
|     10 |    4 | command ID       | Duplicate and response correlation |
|     14 |    4 | session ID       | Active experiment session          |
|     18 |    8 | execute at ticks | Target local time or zero          |
|     26 |    4 | deadline ms      | Sender timeout guidance            |
|     30 |    2 | payload length   | TLV payload bytes                  |
|     32 |    N | payload          | TLV fields                         |

Kind 1 is a command. Kind 2 is a response. The TLV encoding and value types are the same as node v3.

## Command flags

| Bit | Name                | Meaning                            |
| --: | ------------------- | ---------------------------------- |
|   0 | require response    | Sender expects a response          |
|   1 | execute immediately | No future scheduling is requested  |
|   2 | replace existing    | Replace a compatible queued action |
|   3 | safety authorized   | Required for a valve action        |

## Status values

| Value | Name               |
| ----: | ------------------ |
|     0 | ok                 |
|     1 | accepted           |
|     2 | busy               |
|     3 | bad message        |
|     4 | unsupported        |
|     5 | invalid state      |
|     6 | invalid parameter  |
|     7 | not synchronized   |
|     8 | inventory mismatch (session start with no nodes present) |
|     9 | timeout            |
|    10 | transport error    |
|    11 | queue full         |
|    12 | duplicate          |
|    13 | safety lock        |
|    14 | session mismatch   |
|    15 | internal error     |

`accepted` means the command was queued. Final execution is reported with a node `command result` record.

## Core operations

|    Value | Name            | Target                       |
| -------: | --------------- | ---------------------------- |
| `0x0001` | ping            | Any board                    |
| `0x0002` | get inventory   | Hub                       |
| `0x0003` | apply inventory | Retired: Hub answers `unsupported` |
| `0x0004` | identify        | Node                        |
| `0x0005` | get health      | Any board                    |
| `0x0006` | clear faults    | Any board                    |
| `0x0007` | set telemetry   | Hub                       |
| `0x0008` | clock exchange  | Hub                       |
| `0x0100` | start session   | Hub and propagated boards |
| `0x0101` | stop session    | Hub and propagated boards |
| `0x0200` | set RGB         | Node                        |
| `0x0201` | set IR          | Node                        |
| `0x0202` | set audio       | Node                        |
| `0x0203` | actuate valve   | Node                        |
| `0x0204` | configure valve | Node                        |
| `0x0205` | configure pins  | Reserved node extension     |
| `0x0300` | schedule TTL    | TTL generator                    |
| `0x0301` | start TTL train | Hub                       |
| `0x0302` | stop TTL train  | Hub                       |
| `0x0400` | start sync test | Hub                       |
| `0x0401` | stop sync test  | Hub                       |

## Important command fields

|    Value | Name                     | Type            |
| -------: | ------------------------ | --------------- |
| `0x1000` | telemetry level          | Unsigned 8 bit  |
| `0x1001` | UUID list                | Bytes           |
| `0x1003` | identify duration ms     | Unsigned 32 bit |
| `0x1010` | red                      | Unsigned 8 bit  |
| `0x1011` | green                    | Unsigned 8 bit  |
| `0x1012` | blue                     | Unsigned 8 bit  |
| `0x1020` | audio mode               | Unsigned 8 bit  |
| `0x1021` | frequency Hz             | Unsigned 32 bit |
| `0x1022` | low frequency Hz         | Unsigned 32 bit |
| `0x1023` | high frequency Hz        | Unsigned 32 bit |
| `0x1024` | amplitude                | Unsigned 16 bit |
| `0x1025` | duration ms              | Unsigned 32 bit |
| `0x1030` | spike duration us        | Unsigned 32 bit |
| `0x1031` | spike duty per mille     | Unsigned 16 bit |
| `0x1032` | hold duty per mille      | Unsigned 16 bit |
| `0x1033` | maximum on time us       | Unsigned 32 bit |
| `0x1034` | minimum interval us      | Unsigned 32 bit |
| `0x1035` | VLOAD millivolts         | Unsigned 32 bit |
| `0x1040` | TTL frequency millihertz | Unsigned 32 bit |
| `0x1041` | TTL width us             | Unsigned 32 bit |
| `0x1042` | TTL count                | Unsigned 32 bit |
| `0x1043` | sequence                 | Unsigned 32 bit |
| `0x1050` | test command interval ms | Unsigned 32 bit |
| `0x1060` | clock t1 ns              | Unsigned 64 bit |
| `0x1061` | clock t2 ticks           | Unsigned 64 bit |
| `0x1062` | clock t3 ticks           | Unsigned 64 bit |

## Inventory encoding

`get inventory` returns `UUID list` with one 18 byte entry per node that is currently answering polls, in node number order:

| Offset | Size | Meaning                          |
| -----: | ---: | -------------------------------- |
|      0 |   12 | STM32 factory UUID, or zero until Hub has seen the board's boot record |
|     12 |    1 | Transport address, `0x10 + node number` |
|     13 |    1 | Node number                      |
|     14 |    4 | Capability bits                  |

Node numbers are flashed into the boards ([node identity v1](node_identity_v1.md)), so there is nothing to assign. `apply inventory` is retired.

## Sessions

Hub starts a session when at least one node is present, TTL generator is connected, and, when synchronization is required, every present node and TTL generator clock model is valid. Adelie decides which node numbers a run needs and waits for each of them before Record Start.

Commands that affect experiment outputs carry the active session ID. A mismatch is rejected.

If Adelie disconnects, Hub stops the active session and sends outputs to safe state. Health and synchronization continue.

## Audio modes

| Value | Mode                     |
| ----: | ------------------------ |
|     0 | Off                      |
|     1 | Tone                     |
|     2 | Band limited white noise |

## TTL train

Adelie sends frequency in millihertz, width in microseconds, and an optional count. Count zero means continuous. This implementation accepts 0.1 Hz to 10 Hz. Hub schedules each pulse in its clock and converts the target into the TTL generator clock.

The recorded sequence is:

1. Hub emits `TTL scheduled`
2. TTL generator emits `TTL generated`
3. Hub captures the prototype loopback and emits `TTL captured`
4. Hub emits `TTL result` with signed error in nanoseconds

## Sync test

Start sync test:

- Selects full telemetry
- Starts a 1 Hz TTL train
- Sends a safe random RGB command once per second
- Leaves valve commands disabled

Stop sync test restores standard telemetry and stops the diagnostic TTL train.
