#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fairy::wire {

inline std::uint16_t get_u16(const std::uint8_t *value) {
  return static_cast<std::uint16_t>(value[0]) |
         (static_cast<std::uint16_t>(value[1]) << 8U);
}

inline std::uint32_t get_u32(const std::uint8_t *value) {
  return static_cast<std::uint32_t>(value[0]) |
         (static_cast<std::uint32_t>(value[1]) << 8U) |
         (static_cast<std::uint32_t>(value[2]) << 16U) |
         (static_cast<std::uint32_t>(value[3]) << 24U);
}

inline std::uint64_t get_u64(const std::uint8_t *value) {
  return static_cast<std::uint64_t>(get_u32(value)) |
         (static_cast<std::uint64_t>(get_u32(value + 4)) << 32U);
}

inline void put_u16(std::uint8_t *destination, std::uint16_t value) {
  destination[0] = static_cast<std::uint8_t>(value);
  destination[1] = static_cast<std::uint8_t>(value >> 8U);
}

inline void put_u32(std::uint8_t *destination, std::uint32_t value) {
  destination[0] = static_cast<std::uint8_t>(value);
  destination[1] = static_cast<std::uint8_t>(value >> 8U);
  destination[2] = static_cast<std::uint8_t>(value >> 16U);
  destination[3] = static_cast<std::uint8_t>(value >> 24U);
}

inline void put_u64(std::uint8_t *destination, std::uint64_t value) {
  put_u32(destination, static_cast<std::uint32_t>(value));
  put_u32(destination + 4, static_cast<std::uint32_t>(value >> 32U));
}

class Writer {
public:
  Writer(std::uint8_t *destination, std::size_t capacity)
      : destination_(destination), capacity_(capacity) {}

  bool u8(std::uint8_t value) { return bytes(&value, sizeof(value)); }

  bool u16(std::uint16_t value) {
    std::uint8_t encoded[2];
    put_u16(encoded, value);
    return bytes(encoded, sizeof(encoded));
  }

  bool u32(std::uint32_t value) {
    std::uint8_t encoded[4];
    put_u32(encoded, value);
    return bytes(encoded, sizeof(encoded));
  }

  bool u64(std::uint64_t value) {
    std::uint8_t encoded[8];
    put_u64(encoded, value);
    return bytes(encoded, sizeof(encoded));
  }

  bool bytes(const void *source, std::size_t length) {
    if (source == nullptr || position_ + length > capacity_) {
      good_ = false;
      return false;
    }
    std::memcpy(destination_ + position_, source, length);
    position_ += length;
    return true;
  }

  std::size_t size() const { return position_; }
  bool good() const { return good_; }

private:
  std::uint8_t *destination_;
  std::size_t capacity_;
  std::size_t position_{};
  bool good_{true};
};

class Reader {
public:
  Reader(const std::uint8_t *source, std::size_t length)
      : source_(source), length_(length) {}

  bool u8(std::uint8_t &value) { return bytes(&value, sizeof(value)); }

  bool u16(std::uint16_t &value) {
    std::uint8_t encoded[2];
    if (!bytes(encoded, sizeof(encoded))) {
      return false;
    }
    value = get_u16(encoded);
    return true;
  }

  bool u32(std::uint32_t &value) {
    std::uint8_t encoded[4];
    if (!bytes(encoded, sizeof(encoded))) {
      return false;
    }
    value = get_u32(encoded);
    return true;
  }

  bool u64(std::uint64_t &value) {
    std::uint8_t encoded[8];
    if (!bytes(encoded, sizeof(encoded))) {
      return false;
    }
    value = get_u64(encoded);
    return true;
  }

  bool bytes(void *destination, std::size_t length) {
    if (destination == nullptr || position_ + length > length_) {
      good_ = false;
      return false;
    }
    std::memcpy(destination, source_ + position_, length);
    position_ += length;
    return true;
  }

  const std::uint8_t *current() const { return source_ + position_; }
  std::size_t remaining() const { return length_ - position_; }
  bool good() const { return good_; }

private:
  const std::uint8_t *source_;
  std::size_t length_;
  std::size_t position_{};
  bool good_{true};
};

} // namespace fairy::wire
