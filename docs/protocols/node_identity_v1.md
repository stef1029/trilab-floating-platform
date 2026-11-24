# node identity v1

Every node board is given a number, 0 to `RIG_MAX_NODES - 1` (0 to 4 by default), when it is flashed. The number is the board's identity: node N always uses RS485 address `0x10 + N`. It replaces [discovery v1](discovery_protocol_v1.md).

## Giving a board its number

Flash one board at a time with one ST-Link connected:

```bash
python firmware/reward_port/scripts/flash.py --id 3   # firmware and node number
python firmware/reward_port/scripts/flash.py          # firmware only, keeps the number
python firmware/reward_port/scripts/flash.py --read   # report number and UUID
```

`flash.py` without `--id` refuses a board that has no number. Write the number on the board.

## Identity record

The number is stored in the last 2 KB flash page of the STM32G071, at `0x0801F800`. The node linker script (`firmware/reward_port/ld/node_flash.ld`) ends the firmware region before this page, so no firmware image covers it and a firmware-only flash never erases it. A full chip erase does.

The record is 16 bytes, little endian:

| Offset | Size | Field                                   |
| -----: | ---: | --------------------------------------- |
|      0 |    4 | Magic `0x44494E52` ("RNID")             |
|      4 |    1 | Version `1`                             |
|      5 |    1 | Node number                             |
|      6 |    1 | Bitwise inverse of the node number      |
|      7 |    7 | Reserved zero                           |
|     14 |    2 | CRC16 CCITT, initial `0xFFFF`, over bytes 0 to 13 |

The record for node 3 is:

```text
52 4E 49 44 01 03 FC 00 00 00 00 00 00 00 35 A2
```

The codec is `firmware/shared/include/rig_protocol/node_identity.hpp`. `flash.py` writes the same bytes, and the host tests check both against this vector.

## Node behaviour

At boot the node reads the record. If it is valid and the number is below `RIG_MAX_NODES`, the node uses address `0x10 + number` permanently. If the page is erased or the record is invalid, the node never transmits on RS485 and blinks its LED red once a second, so it cannot collide with a numbered board.

The node reports its number and its 96 bit STM32 factory UUID in its boot record. The UUID is information only.

## Hub behaviour

Hub has one slot per node number and polls every slot from boot, so power-up order does not matter:

- A slot that answers becomes present and is polled every 10 ms
- An absent slot is asked again every 500 ms
- A present slot that has not answered for 3 s becomes absent and a running session stops
- A boot record from a present node means the board reset: its clock model restarts and a running session stops

Hub sends an `inventory` record whenever a slot appears, disappears, or its board resets. At boot it broadcasts the discovery `release` message, which tells every node to make its outputs safe.

## Duplicate numbers

Two boards with the same number both answer the same poll and their responses collide on the bus. This shows up as transport errors on that address, not as a specific fault. Keep numbers unique and check a board with `flash.py --read` before fitting it.
