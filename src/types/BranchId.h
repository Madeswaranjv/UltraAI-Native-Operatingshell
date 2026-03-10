#pragma once

#include <cstdint>
#include <string>

namespace ultra::types {

class BranchId {
 public:
  static BranchId generate();
  static BranchId fromString(const std::string& uuid);
  static BranchId nil() noexcept;

  BranchId() noexcept = default;

  std::string toString() const;
  bool isNil() const noexcept;

  bool operator==(const BranchId& other) const noexcept;
  bool operator!=(const BranchId& other) const noexcept;
  bool operator<(const BranchId& other) const noexcept;

 private:
  std::uint64_t m_high{0};
  std::uint64_t m_low{0};

  BranchId(std::uint64_t high, std::uint64_t low) noexcept;
};

}  // namespace ultra::types