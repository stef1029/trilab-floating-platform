# Transport protocol v1

Transport v1 carries complete application messages over BLE and UART with RS485. The frame format is identical on both links.

## Limits

| Item                  |     Limit |
| --------------------- | --------: |
| Raw frame             | 255 bytes |
| Frame header          |  12 bytes |
| Frame CRC             |   2 bytes |
| Fragment payload      | 241 bytes |
| Reassembled message   | 512 bytes |
| Fragments per message |       255 |

All integers use little endian order.

## Raw frame

| Offset | Size | Field          | Meaning                             |
| -----: | ---: | -------------- | ----------------------------------- |
|      0 |    2 | magic          | `0xA7C3`                            |
|      2 |    1 | version        | `1`                                 |
|      3 |    1 | channel        | Application protocol                |
|      4 |    1 | flags          | Transport flags                     |
|      5 |    1 | source         | Sender address                      |
|      6 |    1 | destination    | Receiver address                    |
|      7 |    1 | fragment index | Zero based index                    |
|      8 |    1 | fragment count | Total number of fragments           |
|      9 |    1 | payload length | Bytes in this fragment              |
|     10 |    2 | transfer ID    | Request and response correlation    |
|     12 |    N | payload        | Application bytes                   |
| 12 + N |    2 | CRC16          | CRC16 CCITT over header and payload |

The CRC initial value is `0xFFFF` and the polynomial is `0x1021`.

## Channels

| Value | Protocol    |
| ----: | ----------- |
|     1 | Fairy v3    |
|     2 | Adelie v2   |
|     3 | Magellan v1 |

## Addresses

|         Value | Meaning                       |
| ------------: | ----------------------------- |
|        `0x00` | Unassigned Fairy              |
|        `0x01` | Korora                        |
|        `0x02` | Galapagos                     |
|        `0x03` | Adelie                        |
| `0x10` onward | Assigned Fairy addresses      |
| `0x80` onward | Temporary discovery addresses |
|        `0xFF` | Broadcast                     |

Fairy index 0 maps to `0x10`. Fairy index 1 maps to `0x11`.

## Flags

| Bit | Name            | Meaning                                  |
| --: | --------------- | ---------------------------------------- |
|   0 | ACK required    | Sender asks for confirmation             |
|   1 | Response        | Frame belongs to a response              |
|   2 | Acknowledgement | Confirms a record transfer               |
|   3 | Error           | Transport could not complete the request |
|   4 | First fragment  | First fragment of a message              |
|   5 | Last fragment   | Last fragment of a message               |

The first and last flags are set even for a one fragment message.

## UART encoding

UART uses COBS around each raw frame. One zero byte terminates every encoded frame. No application code reads human serial text from this stream.

The RS485 bus uses:

- 460800 baud
- 8 data bits
- No parity
- 1 stop bit
- Automatic direction control on the prototype
- Korora master polling
- Fairy transmission only after a request or discovery slot

Korora holds one mutex for the complete request, response, and acknowledgement transaction. This prevents two writers from interleaving bytes.

## BLE encoding

Each GATT write or notification contains one raw transport frame. Fragment payload size is reduced to fit the negotiated ATT MTU.

Korora exposes the Adelie service:

| Item    | UUID                                   |
| ------- | -------------------------------------- |
| Service | `a88279d0-7009-4bee-a6f8-e1dc3ff02b92` |
| RX      | `a88279d1-7009-4bee-a6f8-e1dc3ff02b92` |
| TX      | `a88279d2-7009-4bee-a6f8-e1dc3ff02b92` |

Galapagos exposes:

| Item    | UUID                                   |
| ------- | -------------------------------------- |
| Service | `a88278d0-7009-4bee-a6f8-e1dc3ff02b92` |
| RX      | `a88278d1-7009-4bee-a6f8-e1dc3ff02b92` |
| TX      | `a88278d2-7009-4bee-a6f8-e1dc3ff02b92` |

BLE already provides ordered and checked link delivery. Transport CRC remains enabled so the same decoder and recorded evidence are used on both links.

## Reassembly rules

- Fragment zero starts or replaces a transfer
- Every later fragment must have the next index
- Source, destination, channel, transfer ID, and count must stay equal
- Total payload must not exceed 512 bytes
- Invalid or out of order transfers are discarded
