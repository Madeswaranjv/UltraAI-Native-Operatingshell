#pragma once

#include "SnapshotChain.h"
#include <vector>
#include <cstdint>

namespace ultra::memory {

/// Deterministic index over snapshot chain.
/// Snapshots are ordered strictly by snapshot ID.
class TemporalIndex {
 public:
  explicit TemporalIndex(const SnapshotChain& chain);

  /// Return snapshot with ID <= given ID (closest match).
  StateSnapshot queryAtId(uint64_t id) const;

  /// Return all snapshots with ID in [start, end].
  std::vector<StateSnapshot> getChangesBetween(
      uint64_t start,
      uint64_t end) const;

 private:
  const SnapshotChain& chain_;
};

}  // namespace ultra::memory