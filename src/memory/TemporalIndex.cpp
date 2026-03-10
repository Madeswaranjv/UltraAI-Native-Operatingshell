#include "TemporalIndex.h"
#include <algorithm>

namespace ultra::memory {

TemporalIndex::TemporalIndex(const SnapshotChain& chain)
    : chain_(chain) {}

StateSnapshot TemporalIndex::queryAtId(uint64_t id) const {
  auto history = chain_.getHistory();
  if (history.empty())
    return StateSnapshot{};

  // Snapshots are assumed ordered by increasing ID
  StateSnapshot result{};

  for (const auto& snap : history) {
    if (snap.id <= id) {
      result = snap;
    } else {
      break;
    }
  }

  return result;
}

std::vector<StateSnapshot> TemporalIndex::getChangesBetween(
    uint64_t start,
    uint64_t end) const {

  std::vector<StateSnapshot> results;
  auto history = chain_.getHistory();

  for (const auto& snap : history) {
    if (snap.id >= start && snap.id <= end) {
      results.push_back(snap);
    }
  }

  return results;
}

}  // namespace ultra::memory