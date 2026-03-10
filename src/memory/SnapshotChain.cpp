#include "SnapshotChain.h"
#include "../metrics/PerformanceMetrics.h"

#include <algorithm>
#include <chrono>

namespace ultra::memory {

namespace {

std::size_t estimateSnapshotPayloadBytes(const StateSnapshot& snapshot) {
  return sizeof(snapshot.id) + sizeof(snapshot.nodeCount) +
         sizeof(snapshot.edgeCount) + snapshot.snapshotId.size() +
         snapshot.graphHash.size();
}

}  // namespace

void SnapshotChain::append(const StateSnapshot& snapshot) {
  const bool metricsEnabled = metrics::PerformanceMetrics::isEnabled();
  const auto startedAt =
      metricsEnabled ? std::chrono::steady_clock::now()
                     : std::chrono::steady_clock::time_point{};
  snapshots_.push_back(snapshot);
  enforceRetentionCap();

  if (metricsEnabled) {
    metrics::SnapshotMetrics metric;
    metric.operation = "snapshot_chain_append";
    metric.durationMicros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt)
            .count());
    metric.nodeCount = snapshot.nodeCount;
    metric.edgeCount = snapshot.edgeCount;
    metric.snapshotSizeBytes = estimateSnapshotPayloadBytes(snapshot);
    metrics::PerformanceMetrics::recordSnapshotMetric(metric);
  }
}

void SnapshotChain::enforceRetentionCap() {
  while (snapshots_.size() > kMaxSnapshotsRetained) {
    snapshots_.erase(snapshots_.begin());
  }
}

StateSnapshot SnapshotChain::current() const {
  if (snapshots_.empty())
    return StateSnapshot{};
  return snapshots_.back();
}

StateSnapshot SnapshotChain::getSnapshot(uint64_t id) const {
  auto it = std::find_if(
      snapshots_.begin(), snapshots_.end(),
      [&](const StateSnapshot& s) { return s.id == id; });

  if (it != snapshots_.end())
    return *it;

  return StateSnapshot{};
}

StateSnapshot SnapshotChain::getSnapshot(
    const std::string& snapshotId) const {

  auto it = std::find_if(
      snapshots_.begin(), snapshots_.end(),
      [&](const StateSnapshot& s) {
        return s.snapshotId == snapshotId;
      });

  if (it != snapshots_.end())
    return *it;

  return StateSnapshot{};
}

std::vector<StateSnapshot> SnapshotChain::getHistory() const {
  return snapshots_;
}

std::size_t SnapshotChain::size() const noexcept {
  return snapshots_.size();
}

bool SnapshotChain::rollback(uint64_t id) {
  auto it = std::find_if(
      snapshots_.begin(), snapshots_.end(),
      [&](const StateSnapshot& s) { return s.id == id; });

  if (it == snapshots_.end())
    return false;

  snapshots_.erase(it + 1, snapshots_.end());
  return true;
}

bool SnapshotChain::rollback(const std::string& snapshotId) {
  auto it = std::find_if(
      snapshots_.begin(), snapshots_.end(),
      [&](const StateSnapshot& s) {
        return s.snapshotId == snapshotId;
      });

  if (it == snapshots_.end())
    return false;

  snapshots_.erase(it + 1, snapshots_.end());
  return true;
}

void SnapshotChain::clear() {
  snapshots_.clear();
}

}  // namespace ultra::memory
