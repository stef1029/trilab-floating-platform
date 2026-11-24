# discovery protocol v1

> **Retired.** Node boards are now numbered when they are flashed and always use address `0x10 + number`; see [node identity v1](node_identity_v1.md). The only message still sent is the hub's boot-time **release**, which now means "hub restarted: make outputs safe" and no longer clears a node's address. The rest of this document describes the old discovery scheme and is kept for reference.

Discovery assigns temporary and logical RS485 addresses without requiring boards to power up one at a time. Every node uses the 96 bit STM32 factory identifier as its UUID.

## Header

| Offset | Size | Field              |
| -----: | ---: | ------------------ |
|      0 |    2 | Magic `0x4D47`     |
|      2 |    1 | Version `1`        |
|      3 |    1 | Message type       |
|      4 |    4 | Random round nonce |
|      8 |    2 | Payload length     |
|     10 |    2 | Reserved zero      |
|     12 |    N | Payload            |

## Message types

| Value | Name     | Direction                  |
| ----: | -------- | -------------------------- |
|     1 | discover | Hub to broadcast        |
|     2 | offer    | Unassigned node to the hub |
|     3 | assign   | Hub to broadcast        |
|     4 | assigned | Selected node to the hub   |
|     5 | release  | Hub to broadcast        |
|     6 | verify   | Reserved                   |
|     7 | verified | Reserved                   |

## Discovery flow

1. Hub broadcasts release during boot
2. Every node becomes unassigned and turns outputs safe
3. Hub sends discover with a new nonce
4. Every unassigned node selects a deterministic response slot
5. Nodes send offers in their slots
6. Hub gives each offered UUID a temporary address
7. Hub repeats discovery with new nonces until three rounds are empty
8. Adelie receives the complete UUID set
9. Adelie identifies each board with a white LED
10. Adelie sends the exact UUID to zero based node index mapping
11. Hub replaces temporary addresses with logical addresses

Discovery continues once per second so a reset or newly powered board is noticed.

## Slot selection

The default discovery window has 16 slots of 2500 us. The slot is:

```text
CRC16(UUID || nonce) modulo slot count
```

A collision loses both offers for that round. The next round uses a new nonce and therefore changes the slot choice. No power sequence assumption is made.

## Discover payload

| Offset | Size | Field            |
| -----: | ---: | ---------------- |
|      0 |    1 | Slot count       |
|      1 |    2 | Slot duration us |
|      3 |    1 | Round number     |

## Offer payload

| Offset | Size | Field           |
| -----: | ---: | --------------- |
|      0 |   12 | UUID            |
|     12 |    4 | Capability bits |
|     16 |    4 | Boot count      |

Capability bits are:

| Bit | Capability   |
| --: | ------------ |
|   0 | Light gate   |
|   1 | RGB          |
|   2 | Audio        |
|   3 | Valve        |
|   4 | IR           |
|   5 | SYNC capture |

## Assignment payload

| Offset | Size | Field                  |
| -----: | ---: | ---------------------- |
|      0 |   12 | UUID                   |
|     12 |    1 | Transport address      |
|     13 |    1 | Zero based node index |

Only the node with the matching UUID accepts an assignment. It replies from the new address with the same assignment payload.

## Live set behavior

Hub marks a node absent after three seconds without a successful poll. Absent UUIDs leave the inventory sent to Adelie. A known board that returns is reassigned automatically. A different replacement UUID returns Adelie to configuration because the complete set no longer matches the saved file.
