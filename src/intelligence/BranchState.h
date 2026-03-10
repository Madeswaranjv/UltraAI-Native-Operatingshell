#pragma once
//E:\Projects\Ultra\src\intelligence\BranchState.h
#include <cstdint>
#include <string>

namespace ultra::intelligence {

/// Represents the high-level cognitive state of a reasoning branch.
enum class BranchState : std::uint8_t {
  Unknown = 0,
  Active = 1,
  Suspended = 2,
  Merged = 3,
  Archived = 4,
  RolledBack = 5
};

/// Convert BranchState to string for JSON serialization.
inline std::string toString(BranchState state) {
  switch (state) {
    case BranchState::Active: return "Active";
    case BranchState::Suspended: return "Suspended";
    case BranchState::Merged: return "Merged";
    case BranchState::Archived: return "Archived";
    case BranchState::RolledBack: return "RolledBack";
    default: return "Unknown";
  }
}

/// Parse BranchState from string.
inline BranchState fromString(const std::string& str) {
  if (str == "Active") return BranchState::Active;
  if (str == "Suspended") return BranchState::Suspended;
  if (str == "Merged") return BranchState::Merged;
  if (str == "Archived") return BranchState::Archived;
  if (str == "RolledBack") return BranchState::RolledBack;
  return BranchState::Unknown;
}

}  // namespace ultra::intelligence
