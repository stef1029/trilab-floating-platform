# Fairy record protocol v3

Fairy v3 is the record collection protocol. A record describes measured state, an action, health, or timing evidence. Records use a fixed header and a flexible TLV payload.

The protocol does not require every record type to have a different C struct. New fields can be added without changing the fixed header.

## Record request

On RS485, Korora requests one record by sending an empty Fairy message to an assigned Fairy address.

The Fairy response contains:

- The oldest queued record when one exists
- An empty Fairy transport response when the queue is empty

Korora acknowledges the transfer ID after it accepts the record. The Fairy removes the record only after this acknowledgement. A lost response can therefore be requested again without silently losing the record.

## Record header

| Offset | Size | Field           | Meaning                      |
| -----: | ---: | --------------- | ---------------------------- |
|      0 |    2 | magic           | `0xFA13`                     |
|      2 |    1 | version         | `3`                          |
|      3 |    1 | header size     | `32`                         |
|      4 |    2 | record type     | Record type value            |
|      6 |    2 | flags           | Record flags                 |
|      8 |    4 | record ID       | Monotonic ID from the source |
|     12 |    4 | session ID      | Zero outside a run           |
|     16 |    8 | timestamp ticks | Source local timestamp       |
|     24 |    4 | clock Hz        | Usually `16000000`           |
|     28 |    2 | payload length  | TLV payload bytes            |
|     30 |    2 | reserved        | Must be zero                 |
|     32 |    N | payload         | TLV fields                   |

The maximum payload is 256 bytes.

## TLV field

| Offset | Size | Field        |
| -----: | ---: | ------------ |
|      0 |    2 | Tag          |
|      2 |    1 | Value type   |
|      3 |    1 | Value length |
|      4 |    N | Value        |

Value types are:

| Value | Type            |
| ----: | --------------- |
|     1 | Unsigned 8 bit  |
|     2 | Unsigned 16 bit |
|     3 | Unsigned 32 bit |
|     4 | Unsigned 64 bit |
|     5 | Signed 32 bit   |
|     6 | Signed 64 bit   |
|     7 | Float 32        |
|     8 | Float 64        |
|     9 | Boolean         |
|    10 | Bytes           |
|    11 | UTF8 string     |

Readers must ignore unknown tags with a valid length and type. Writers must not change the meaning of an existing tag.

## Record types

|    Value | Name              | Purpose                                     |
| -------: | ----------------- | ------------------------------------------- |
| `0x0001` | boot              | Reset and firmware information              |
| `0x0002` | health            | Queue, uptime, sensor, and transport health |
| `0x0003` | inventory         | Live UUID set change                        |
| `0x0004` | link quality      | RSSI, retry, and connection information     |
| `0x0100` | sync observation  | Raw local SYNC or BLE anchor timestamp      |
| `0x0101` | sync quality      | RMS, skew, points, and generation           |
| `0x0102` | clock model reset | Model loss reason                           |
| `0x0103` | clock pair        | Local and reference sample pair             |
| `0x0200` | digital input     | Captured digital edge                       |
| `0x0201` | output change     | RGB, audio, IR, or valve change             |
| `0x0202` | TTL scheduled     | Target selected by Korora                   |
| `0x0203` | TTL generated     | Galapagos hardware compare event            |
| `0x0204` | TTL captured      | Korora loopback capture                     |
| `0x0205` | TTL result        | Final target error                          |
| `0x0206` | light gate        | Fairy beam interruption                     |
| `0x0300` | command result    | Actual command execution result             |
| `0x0400` | fault             | Safety or hardware fault                    |
| `0x0500` | test marker       | Diagnostic marker                           |

## Important fields

|    Value | Name             | Type                      |
| -------: | ---------------- | ------------------------- |
| `0x0001` | UUID             | Bytes                     |
| `0x0002` | logical slot     | Unsigned 8 bit            |
| `0x0003` | link address     | Unsigned 8 bit            |
| `0x0007` | uptime ms        | Unsigned 32 bit           |
| `0x0008` | queue depth      | Unsigned 8 bit            |
| `0x000A` | dropped records  | Unsigned 32 bit           |
| `0x000B` | transport errors | Unsigned 32 bit           |
| `0x0010` | command ID       | Unsigned 32 bit           |
| `0x0013` | requested ticks  | Unsigned 64 bit           |
| `0x0014` | actual ticks     | Unsigned 64 bit           |
| `0x0016` | sequence         | Unsigned 32 bit           |
| `0x0019` | value            | Type chosen by the record |
| `0x001A` | reference ticks  | Unsigned 64 bit           |
| `0x0030` | RMS ns           | Signed 64 bit             |
| `0x0031` | skew ppb         | Signed 64 bit             |
| `0x0032` | model points     | Unsigned 8 bit            |
| `0x0040` | RSSI dBm         | Signed 32 bit             |

`timestamp ticks` always remains in the source clock. When Korora has a valid clock model it adds `reference ticks`. This keeps local evidence and converted time separate.

## Flags

| Bit | Name              | Meaning                                   |
| --: | ----------------- | ----------------------------------------- |
|   0 | critical          | Forward at every telemetry level          |
|   1 | first after reset | First record after boot                   |
|   2 | synchronized      | A valid reference conversion exists       |
|   3 | scheduled         | Action used a requested future time       |
|   4 | actual time       | Timestamp came from hardware or execution |
|   5 | loss latched      | Capture or queue loss occurred            |

## Telemetry levels

|      Level | Records                                                         |
| ---------: | --------------------------------------------------------------- |
| 0 critical | Events, command results, TTL evidence, and faults               |
| 1 standard | Critical plus boot, health, inventory, links, and clock quality |
|     2 full | Standard plus raw SYNC observations and clock pairs             |

Changing telemetry changes forwarding from Korora to Adelie. It does not disable local event capture.
