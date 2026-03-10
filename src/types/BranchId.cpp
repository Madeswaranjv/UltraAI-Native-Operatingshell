#include "BranchId.h"

#include <iomanip>
#include <sstream>
#include <random>

namespace ultra::types {

BranchId::BranchId(std::uint64_t high, std::uint64_t low) noexcept
    : m_high(high), m_low(low) {}

BranchId BranchId::generate() {
  // UUID v4 generation (random-based)
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint64_t> dist;

  std::uint64_t high = dist(rng);
  std::uint64_t low = dist(rng);

  // Set version to 4 (UUID v4)
  high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;

  // Set variant bits (RFC 4122)
  low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  return BranchId(high, low);
}

BranchId BranchId::fromString(const std::string& uuid) {
  if (uuid.size() != 36) {
    return BranchId::nil();
  }

  std::string hex;
  hex.reserve(32);

  for (char c : uuid) {
    if (c != '-') {
      hex.push_back(c);
    }
  }

  if (hex.size() != 32) {
    return BranchId::nil();
  }

  try {
    std::uint64_t high = std::stoull(hex.substr(0, 16), nullptr, 16);
    std::uint64_t low = std::stoull(hex.substr(16, 16), nullptr, 16);
    return BranchId(high, low);
  } catch (...) {
    return BranchId::nil();
  }
}

BranchId BranchId::nil() noexcept {
  return BranchId(0, 0);
}

std::string BranchId::toString() const {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');

  std::uint32_t a = static_cast<std::uint32_t>(m_high >> 32);
  std::uint16_t b = static_cast<std::uint16_t>(m_high >> 16);
  std::uint16_t c = static_cast<std::uint16_t>(m_high);
  std::uint16_t d = static_cast<std::uint16_t>(m_low >> 48);
  std::uint64_t e = m_low & 0x0000FFFFFFFFFFFFULL;

  oss << std::setw(8) << a << '-'
      << std::setw(4) << b << '-'
      << std::setw(4) << c << '-'
      << std::setw(4) << d << '-'
      << std::setw(12) << e;

  return oss.str();
}

bool BranchId::isNil() const noexcept {
  return m_high == 0 && m_low == 0;
}

bool BranchId::operator==(const BranchId& other) const noexcept {
  return m_high == other.m_high && m_low == other.m_low;
}

bool BranchId::operator!=(const BranchId& other) const noexcept {
  return !(*this == other);
}

bool BranchId::operator<(const BranchId& other) const noexcept {
  if (m_high != other.m_high) {
    return m_high < other.m_high;
  }
  return m_low < other.m_low;
}

}  // namespace ultra::types