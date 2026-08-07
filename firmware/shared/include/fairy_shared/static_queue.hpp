#pragma once

#include <array>
#include <cstddef>

namespace fairy {

template <typename Value, std::size_t Capacity> class StaticQueue {
public:
  static_assert(Capacity > 0, "queue capacity must be positive");

  bool push(const Value &value) {
    if (full()) {
      return false;
    }
    values_[write_] = value;
    write_ = (write_ + 1U) % Capacity;
    ++size_;
    return true;
  }

  bool pop(Value &value) {
    if (empty()) {
      return false;
    }
    value = values_[read_];
    read_ = (read_ + 1U) % Capacity;
    --size_;
    return true;
  }

  Value *front() { return empty() ? nullptr : &values_[read_]; }
  const Value *front() const { return empty() ? nullptr : &values_[read_]; }

  bool drop_front() {
    if (empty()) {
      return false;
    }
    read_ = (read_ + 1U) % Capacity;
    --size_;
    return true;
  }

  void clear() {
    read_ = 0;
    write_ = 0;
    size_ = 0;
  }

  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == Capacity; }
  std::size_t size() const { return size_; }
  constexpr std::size_t capacity() const { return Capacity; }

private:
  std::array<Value, Capacity> values_{};
  std::size_t read_{};
  std::size_t write_{};
  std::size_t size_{};
};

} // namespace fairy
