#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace ultra::types {

/// Monotonic timestamp wrapper with ISO-8601 serialization.
class Timestamp {
 public:
  using Clock = std::chrono::system_clock;
  using TimePoint = Clock::time_point;

  /// Create a timestamp representing "now".
  static Timestamp now() noexcept;

  /// Create a timestamp from epoch milliseconds.
  static Timestamp fromEpochMs(std::int64_t ms) noexcept;

  /// Create a timestamp from an ISO-8601 string.
  /// Returns a zero timestamp if parsing fails.
  static Timestamp fromIso8601(const std::string& iso);

  /// Default constructor: epoch zero.
  Timestamp() noexcept = default;

  /// Epoch milliseconds since 1970-01-01T00:00:00Z.
  std::int64_t epochMs() const noexcept;

  /// ISO-8601 formatted string (e.g. "2026-02-21T16:30:00.000Z").
  std::string toIso8601() const;
  std::string toISO8601() const { return toIso8601(); }

  bool operator==(const Timestamp& other) const noexcept;
  bool operator!=(const Timestamp& other) const noexcept;
  bool operator<(const Timestamp& other) const noexcept;
  bool operator<=(const Timestamp& other) const noexcept;
  bool operator>(const Timestamp& other) const noexcept;
  bool operator>=(const Timestamp& other) const noexcept;

 private:
  explicit Timestamp(TimePoint tp) noexcept;
  TimePoint m_timePoint{};
};

}  // namespace ultra::types
