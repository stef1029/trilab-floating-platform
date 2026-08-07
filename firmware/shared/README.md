# Shared C++ library

The shared folder contains code used by Korora, Galapagos, and Fairy. It has no heap requirement and does not depend on Zephyr or STM32 HAL.

## Modules

| Module                  | Purpose                                            |
| ----------------------- | -------------------------------------------------- |
| `system_config.hpp`     | Maximum boards, addresses, rates, and debug switch |
| `bytes.hpp`             | Checked little endian readers and writers          |
| `crc16.hpp`             | CRC16 CCITT                                        |
| `static_queue.hpp`      | Fixed capacity queue                               |
| `transport.hpp`         | Transport framing, COBS, and reassembly            |
| `tlv.hpp`               | Flexible typed fields                              |
| `fairy_protocol.hpp`    | Fairy v3 record codec                              |
| `adelie_protocol.hpp`   | Adelie v2 command codec                            |
| `magellan_protocol.hpp` | Discovery and assignment codec                     |
| `clock_model.hpp`       | Fixed window affine clock model                    |

## Configuration

The main build values are at the top of `include/fairy_shared/system_config.hpp`.

```cpp
#define FAIRY_MAX_BOARDS 6
#define FAIRY_ENABLE_DEBUG_STREAM 1
```

They can also be supplied as compiler definitions. The wire format allows at most 14 Fairies in one inventory response. This project defaults to six.

## PlatformIO

`library.json` lets the Fairy project include this folder through `lib_deps`.

## Host tests

The test builds the protocol code with the host C++ compiler and checks:

- Transport encode and decode
- COBS framing
- Fragment reassembly
- Fairy record round trip
- Adelie command round trip
- Magellan UUID assignment
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
