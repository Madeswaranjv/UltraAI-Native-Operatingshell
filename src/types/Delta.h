#pragma once

#include "Timestamp.h"

#include <string>

namespace ultra::types {

/// Describes how a value changed.
enum class ChangeType {
  Added,
  Removed,
  Modified,
  Renamed,
  Unchanged
};

/// Convert ChangeType to string.
inline std::string changeTypeToString(ChangeType type) {
  switch (type) {
    case ChangeType::Added:     return "added";
    case ChangeType::Removed:   return "removed";
    case ChangeType::Modified:  return "modified";
    case ChangeType::Renamed:   return "renamed";
    case ChangeType::Unchanged: return "unchanged";
  }
  return "unknown";
}

/// Generic delta between two states of a value.
///
/// Used by the diff engine, memory layer, and change tracker.
///
/// Usage:
///   Delta<std::string> d;
///   d.before = "old_name";
///   d.after  = "new_name";
///   d.changeType = ChangeType::Modified;
///   d.impactScore = 0.7;
template <typename T>
struct Delta {
  /// State before the change (empty/default if Added).
  T before{};

  /// State after the change (empty/default if Removed).
  T after{};

  /// What kind of change this represents.
  ChangeType changeType{ChangeType::Unchanged};

  /// Quantified impact score [0.0, 1.0].
  double impactScore{0.0};

  /// When this change was detected.
  Timestamp timestamp;

  /// Optional human-readable description of the change.
  std::string description;
};

}  // namespace ultra::types
