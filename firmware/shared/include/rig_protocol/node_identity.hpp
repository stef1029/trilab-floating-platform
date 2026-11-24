#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Node identity page.
 *
 * Each node board is given a number (0 to max_nodes - 1) when it is flashed.
 * The number is stored in the last 2 KB flash page, outside the firmware
 * image, so flashing new firmware never changes it. The number is the
 * board's identity: its RS485 address is node_address(number).
 *
 * Record layout, little endian, at the start of the page:
 *
 *   offset  size  field
 *        0     4  magic 0x44494E52 ("RNID")
 *        4     1  version (1)
 *        5     1  node id
 *        6     1  bitwise inverse of node id
 *        7     7  reserved, zero
 *       14     2  CRC16-CCITT (init 0xFFFF) over bytes 0 to 13
 *
 * Erased flash reads 0xFF, which fails the magic check, so a board that was
 * never given a number has no identity. The flashing script writes the same
 * bytes (firmware/reward_port/scripts/flash.py); keep the two in step.
 */

namespace rig::identity {

inline constexpr std::uint32_t page_address = 0x0801F800U;
inline constexpr std::size_t page_size = 2048U;
inline constexpr std::uint32_t magic = 0x44494E52U;
inline constexpr std::uint8_t version = 1U;
inline constexpr std::size_t record_size = 16U;

/* Writes the 16 byte record for node_id. Returns 0 if node_id is out of
 * range or the buffer is too small. */
std::size_t encode_identity(std::uint8_t node_id, std::uint8_t *destination,
                            std::size_t capacity);

/* Validates a record and extracts the node id. Returns false for erased,
 * corrupt or out of range records. */
bool decode_identity(const std::uint8_t *record, std::size_t length,
                     std::uint8_t &node_id);

} // namespace rig::identity
