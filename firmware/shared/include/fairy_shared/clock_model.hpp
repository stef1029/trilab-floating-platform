#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace fairy::time {

struct ClockPoint {
  std::uint64_t local_ticks{};
  std::uint64_t reference_ticks{};
};

struct ClockQuality {
  bool valid{};
  std::size_t points{};
  double slope{};
  double rms_ticks{};
  std::int64_t skew_ppb{};
  std::uint32_t generation{};
  std::uint32_t consecutive_rejections{};
};

template <std::size_t Window = 16> class AffineClockModel {
public:
  static_assert(Window >= 4,
                "clock model window must contain at least 4 points");

  bool add(std::uint64_t local_ticks, std::uint64_t reference_ticks) {
    ClockPoint candidate{local_ticks, reference_ticks};

    if (count_ >= 2U && valid_) {
      const double predicted = predict_double(local_ticks);
      const double error =
          std::fabs(predicted - static_cast<double>(reference_ticks));
      const double gate =
          std::fmax(admission_floor_ticks_, rms_ticks_ * admission_multiplier_);
      if (error > gate) {
        ++consecutive_rejections_;
        if (consecutive_rejections_ >= maximum_consecutive_rejections_) {
          reset();
        }
        return false;
      }
    }

    consecutive_rejections_ = 0;
    if (count_ < Window) {
      points_[count_++] = candidate;
    } else {
      for (std::size_t i = 1; i < Window; ++i) {
        points_[i - 1U] = points_[i];
      }
      points_[Window - 1U] = candidate;
    }
    fit();
    return true;
  }

  void reset() {
    count_ = 0;
    valid_ = false;
    slope_ = 1.0;
    rms_ticks_ = 0.0;
    local_reference_ = 0;
    reference_at_local_reference_ = 0.0;
    consecutive_rejections_ = 0;
    ++generation_;
  }

  bool predict(std::uint64_t local_ticks, std::uint64_t &reference_out) const {
    if (!valid_) {
      return false;
    }
    const double value = predict_double(local_ticks);
    if (value < 0.0 || value > static_cast<double>(UINT64_MAX)) {
      return false;
    }
    reference_out = static_cast<std::uint64_t>(std::llround(value));
    return true;
  }

  bool inverse(std::uint64_t reference_ticks, std::uint64_t &local_out) const {
    if (!valid_ || slope_ <= 0.0) {
      return false;
    }
    const double value =
        static_cast<double>(local_reference_) +
        (static_cast<double>(reference_ticks) - reference_at_local_reference_) /
            slope_;
    if (value < 0.0 || value > static_cast<double>(UINT64_MAX)) {
      return false;
    }
    local_out = static_cast<std::uint64_t>(std::llround(value));
    return true;
  }

  ClockQuality quality(double nominal_slope = 1.0) const {
    ClockQuality result;
    result.valid = valid_;
    result.points = count_;
    result.slope = slope_;
    result.rms_ticks = rms_ticks_;
    result.skew_ppb = nominal_slope == 0.0
                          ? 0
                          : static_cast<std::int64_t>(std::llround(
                                (slope_ / nominal_slope - 1.0) * 1.0e9));
    result.generation = generation_;
    result.consecutive_rejections = consecutive_rejections_;
    return result;
  }

  void set_admission(double floor_ticks, double multiplier,
                     std::uint32_t maximum_rejections) {
    admission_floor_ticks_ = floor_ticks;
    admission_multiplier_ = multiplier;
    maximum_consecutive_rejections_ = maximum_rejections;
  }

private:
  double predict_double(std::uint64_t local_ticks) const {
    return reference_at_local_reference_ +
           slope_ * signed_delta(local_ticks, local_reference_);
  }

  static double signed_delta(std::uint64_t value, std::uint64_t reference) {
    return value >= reference ? static_cast<double>(value - reference)
                              : -static_cast<double>(reference - value);
  }

  void fit() {
    if (count_ < 4U) {
      valid_ = false;
      return;
    }

    local_reference_ = points_[count_ - 1U].local_ticks;
    const std::uint64_t reference_base = points_[count_ - 1U].reference_ticks;

    double x_mean = 0.0;
    double y_mean = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
      x_mean += signed_delta(points_[i].local_ticks, local_reference_);
      y_mean += signed_delta(points_[i].reference_ticks, reference_base);
    }
    x_mean /= static_cast<double>(count_);
    y_mean /= static_cast<double>(count_);

    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
      const double x = signed_delta(points_[i].local_ticks, local_reference_);
      const double y = signed_delta(points_[i].reference_ticks, reference_base);
      numerator += (x - x_mean) * (y - y_mean);
      denominator += (x - x_mean) * (x - x_mean);
    }

    if (denominator <= 0.0) {
      valid_ = false;
      return;
    }

    const double candidate_slope = numerator / denominator;
    if (candidate_slope <= 0.0 || candidate_slope < 0.95 ||
        candidate_slope > 1.05) {
      valid_ = false;
      return;
    }

    const double relative_intercept = y_mean - candidate_slope * x_mean;
    const double candidate_reference =
        static_cast<double>(reference_base) + relative_intercept;

    double square_sum = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
      const double x = signed_delta(points_[i].local_ticks, local_reference_);
      const double predicted = candidate_reference + candidate_slope * x;
      const double error =
          predicted - static_cast<double>(points_[i].reference_ticks);
      square_sum += error * error;
    }

    slope_ = candidate_slope;
    reference_at_local_reference_ = candidate_reference;
    rms_ticks_ = std::sqrt(square_sum / static_cast<double>(count_));
    valid_ = true;
    ++generation_;
  }

  std::array<ClockPoint, Window> points_{};
  std::size_t count_{};
  bool valid_{};
  double slope_{1.0};
  double rms_ticks_{};
  std::uint64_t local_reference_{};
  double reference_at_local_reference_{};
  std::uint32_t generation_{};
  std::uint32_t consecutive_rejections_{};
  double admission_floor_ticks_{3200.0};
  double admission_multiplier_{6.0};
  std::uint32_t maximum_consecutive_rejections_{3};
};

} // namespace fairy::time
