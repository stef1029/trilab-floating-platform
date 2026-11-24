# Shared C++ library

The shared folder contains code used by the hub, TTL generator, and node. It has no heap requirement and does not depend on Zephyr or STM32 HAL.

## Modules

| Module                  | Purpose                                            |
| ----------------------- | -------------------------------------------------- |
| `system_config.hpp`     | Maximum boards, addresses, rates, and debug switch |
| `bytes.hpp`             | Checked little endian readers and writers          |
| `crc16.hpp`             | CRC16 CCITT                                        |
| `static_queue.hpp`      | Fixed capacity queue                               |
| `transport.hpp`         | Transport framing, COBS, and reassembly            |
| `tlv.hpp`               | Flexible typed fields                              |
| `record_protocol.hpp`    | Record v3 record codec                              |
| `command_protocol.hpp`   | Command v2 command codec                            |
| `discovery_protocol.hpp` | Discovery and assignment codec                     |
| `clock_model.hpp`       | Fixed window affine clock model                    |

## Configuration

The main build values are at the top of `include/rig_protocol/system_config.hpp`.

```cpp
#define RIG_MAX_NODES 5
#define NODE_ENABLE_DEBUG_STREAM 1
```

They can also be supplied as compiler definitions. The wire format allows at most 14 Nodes in one inventory response. This project defaults to five.

## PlatformIO

`library.json` lets the node project include this folder through `lib_deps`.

## Host tests

The test builds the protocol code with the host C++ compiler and checks:

- Transport encode and decode
- COBS framing
- Fragment reassembly
- record round trip
- Adelie command round trip
- Discovery UUID assignment
- Clock model fit and inverse conversion

Run:

```bash
bash tests/run_host_tests.sh
```

The test uses C++17 with warnings enabled.

## Extension rules

- Add application fields as new TLV tags
- Never reuse an existing numeric meaning
- Keep unknown field handling permissive
- Increase an application protocol version for an incompatible change
- Increase Transport only when frame interpretation changes
- Keep all fixed wire structs independent of compiler packing
