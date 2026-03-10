#include "Timestamp.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace ultra::types {

Timestamp::Timestamp(TimePoint tp) noexcept : m_timePoint(tp) {}

Timestamp Timestamp::now() noexcept {
  return Timestamp(Clock::now());
}

Timestamp Timestamp::fromEpochMs(std::int64_t ms) noexcept {
  return Timestamp(TimePoint(std::chrono::milliseconds(ms)));
}

Timestamp Timestamp::fromIso8601(const std::string& iso) {
  // Parse "YYYY-MM-DDTHH:MM:SS" or "YYYY-MM-DDTHH:MM:SS.mmmZ"
  if (iso.size() < 19) {
    return Timestamp{};
  }

  std::tm tm{};
  tm.tm_isdst = -1;

  // Parse date-time components manually for portability.
  auto parseInt = [&](std::size_t pos, std::size_t len) -> int {
    return std::stoi(iso.substr(pos, len));
  };

  try {
    tm.tm_year = parseInt(0, 4) - 1900;
    tm.tm_mon = parseInt(5, 2) - 1;
    tm.tm_mday = parseInt(8, 2);
    tm.tm_hour = parseInt(11, 2);
    tm.tm_min = parseInt(14, 2);
    tm.tm_sec = parseInt(17, 2);
  } catch (...) {
    return Timestamp{};
  }

#if defined(_WIN32)
  std::time_t t = _mkgmtime(&tm);
#else
  std::time_t t = timegm(&tm);
#endif
  if (t == static_cast<std::time_t>(-1)) {
    return Timestamp{};
  }

  auto tp = Clock::from_time_t(t);

  // Parse optional milliseconds (.mmm).
  if (iso.size() >= 23 && iso[19] == '.') {
    try {
      int ms = parseInt(20, 3);
      tp += std::chrono::milliseconds(ms);
    } catch (...) {
      // Ignore invalid milliseconds.
    }
  }

  return Timestamp(tp);
}

std::int64_t Timestamp::epochMs() const noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             m_timePoint.time_since_epoch())
      .count();
}

std::string Timestamp::toIso8601() const {
  auto epochMillis = epochMs();
  auto secs = epochMillis / 1000;
  auto ms = epochMillis % 1000;
  if (ms < 0) {
    ms += 1000;
    --secs;
  }

  std::time_t t = static_cast<std::time_t>(secs);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif

  std::ostringstream oss;
  oss << std::setfill('0')
      << std::setw(4) << (tm.tm_year + 1900) << '-'
      << std::setw(2) << (tm.tm_mon + 1) << '-'
      << std::setw(2) << tm.tm_mday << 'T'
      << std::setw(2) << tm.tm_hour << ':'
      << std::setw(2) << tm.tm_min << ':'
      << std::setw(2) << tm.tm_sec << '.'
      << std::setw(3) << ms << 'Z';
  return oss.str();
}

bool Timestamp::operator==(const Timestamp& other) const noexcept {
  return m_timePoint == other.m_timePoint;
}
bool Timestamp::operator!=(const Timestamp& other) const noexcept {
  return m_timePoint != other.m_timePoint;
}
bool Timestamp::operator<(const Timestamp& other) const noexcept {
  return m_timePoint < other.m_timePoint;
}
bool Timestamp::operator<=(const Timestamp& other) const noexcept {
  return m_timePoint <= other.m_timePoint;
}
bool Timestamp::operator>(const Timestamp& other) const noexcept {
  return m_timePoint > other.m_timePoint;
}
bool Timestamp::operator>=(const Timestamp& other) const noexcept {
  return m_timePoint >= other.m_timePoint;
}

}  // namespace ultra::types
