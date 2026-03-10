#pragma once

#include "StateSnapshot.h"
#include <cstddef>
#include <vector>
#include <cstdint>

namespace ultra::memory {

class SnapshotChain {
 public:
  static constexpr std::size_t kMaxSnapshotsRetained = 3U;

  void append(const StateSnapshot& snapshot);
  void enforceRetentionCap();

  StateSnapshot current() const;

  StateSnapshot getSnapshot(uint64_t id) const;
  StateSnapshot getSnapshot(const std::string& snapshotId) const;

  std::vector<StateSnapshot> getHistory() const;
  [[nodiscard]] std::size_t size() const noexcept;

  bool rollback(uint64_t id);
  bool rollback(const std::string& snapshotId);

  void clear();

 private:
  std::vector<StateSnapshot> snapshots_;
};

}  // namespace ultra::memory
