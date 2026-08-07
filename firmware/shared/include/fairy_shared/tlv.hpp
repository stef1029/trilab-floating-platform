#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "fairy_shared/bytes.hpp"

namespace fairy::protocol {

enum class ValueType : std::uint8_t {
  u8 = 1,
  u16 = 2,
  u32 = 3,
  u64 = 4,
  i32 = 5,
  i64 = 6,
  f32 = 7,
  f64 = 8,
  boolean = 9,
  bytes = 10,
  string = 11,
};

struct FieldView {
  std::uint16_t tag{};
  ValueType type{ValueType::bytes};
  const std::uint8_t *value{};
  std::uint8_t length{};
};

class TlvWriter {
public:
  TlvWriter(std::uint8_t *destination, std::size_t capacity)
      : writer_(destination, capacity) {}

  bool raw(std::uint16_t tag, ValueType type, const void *value,
           std::uint8_t length) {
    return writer_.u16(tag) && writer_.u8(static_cast<std::uint8_t>(type)) &&
           writer_.u8(length) && (length == 0U || writer_.bytes(value, length));
  }

  bool u8(std::uint16_t tag, std::uint8_t value) {
    return raw(tag, ValueType::u8, &value, sizeof(value));
  }

  bool u16(std::uint16_t tag, std::uint16_t value) {
    std::uint8_t encoded[2];
    wire::put_u16(encoded, value);
    return raw(tag, ValueType::u16, encoded, sizeof(encoded));
  }

  bool u32(std::uint16_t tag, std::uint32_t value) {
    std::uint8_t encoded[4];
    wire::put_u32(encoded, value);
    return raw(tag, ValueType::u32, encoded, sizeof(encoded));
  }

  bool u64(std::uint16_t tag, std::uint64_t value) {
    std::uint8_t encoded[8];
    wire::put_u64(encoded, value);
    return raw(tag, ValueType::u64, encoded, sizeof(encoded));
  }

  bool i32(std::uint16_t tag, std::int32_t value) {
    return u32_typed(tag, static_cast<std::uint32_t>(value), ValueType::i32);
  }

  bool i64(std::uint16_t tag, std::int64_t value) {
    std::uint8_t encoded[8];
    wire::put_u64(encoded, static_cast<std::uint64_t>(value));
    return raw(tag, ValueType::i64, encoded, sizeof(encoded));
  }

  bool boolean(std::uint16_t tag, bool value) {
    const std::uint8_t encoded = value ? 1U : 0U;
    return raw(tag, ValueType::boolean, &encoded, sizeof(encoded));
  }

  bool bytes(std::uint16_t tag, const void *value, std::uint8_t length) {
    return raw(tag, ValueType::bytes, value, length);
  }

  bool string(std::uint16_t tag, const char *value) {
    if (value == nullptr) {
      return false;
    }
    const std::size_t length = std::strlen(value);
    if (length > 255U) {
      return false;
    }
    return raw(tag, ValueType::string, value,
               static_cast<std::uint8_t>(length));
  }

  std::size_t size() const { return writer_.size(); }
  bool good() const { return writer_.good(); }

private:
  bool u32_typed(std::uint16_t tag, std::uint32_t value, ValueType type) {
    std::uint8_t encoded[4];
    wire::put_u32(encoded, value);
    return raw(tag, type, encoded, sizeof(encoded));
  }

  wire::Writer writer_;
};

class TlvReader {
public:
  TlvReader(const std::uint8_t *source, std::size_t length)
      : reader_(source, length) {}

  bool next(FieldView &field) {
    if (reader_.remaining() == 0U) {
      return false;
    }

    std::uint8_t raw_type{};
    if (!reader_.u16(field.tag) || !reader_.u8(raw_type) ||
        !reader_.u8(field.length) || reader_.remaining() < field.length) {
      malformed_ = true;
      return false;
    }
    field.type = static_cast<ValueType>(raw_type);
    field.value = reader_.current();

    std::uint8_t ignored[255];
    return reader_.bytes(ignored, field.length);
  }

  bool malformed() const { return malformed_ || !reader_.good(); }

  static bool as_u8(const FieldView &field, std::uint8_t &value) {
    if (field.type != ValueType::u8 || field.length != 1U) {
      return false;
    }
    value = field.value[0];
    return true;
  }

  static bool as_u16(const FieldView &field, std::uint16_t &value) {
    if (field.type != ValueType::u16 || field.length != 2U) {
      return false;
    }
    value = wire::get_u16(field.value);
    return true;
  }

  static bool as_u32(const FieldView &field, std::uint32_t &value) {
    if (field.type != ValueType::u32 || field.length != 4U) {
      return false;
    }
    value = wire::get_u32(field.value);
    return true;
  }

  static bool as_u64(const FieldView &field, std::uint64_t &value) {
    if (field.type != ValueType::u64 || field.length != 8U) {
      return false;
    }
    value = wire::get_u64(field.value);
    return true;
  }

private:
  wire::Reader reader_;
  bool malformed_{};
};

} // namespace fairy::protocol
