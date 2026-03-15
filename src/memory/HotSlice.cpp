#include "HotSlice.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
//E:\Projects\Ultra\src\memory\HotSlice.cpp
namespace ultra::memory {

namespace {

double clampFinite(const double value,
                   const double minValue,
                   const double maxValue) {
  if (!std::isfinite(value)) {
    return minValue;
  }
  return std::clamp(value, minValue, maxValue);
}

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

HotSlice::HotSlice(std::size_t maxSize)
    : maxSize_(std::max<std::size_t>(1U, maxSize)) {}

// ---------------------------------------------------------------------------
// Static capacity formula
// ---------------------------------------------------------------------------

std::size_t HotSlice::computeCapacity(
    const std::size_t graphNodeCount,
    const std::size_t avgSnapshotNodes) noexcept {
  const std::size_t workingSetCover = avgSnapshotNodes * 2U;
  const std::size_t graphCover     = graphNodeCount / 20U;
  return std::max({workingSetCover, graphCover, kMinHotSliceEntries});
}

// ---------------------------------------------------------------------------
// Access / promotion / demotion
// ---------------------------------------------------------------------------

void HotSlice::recordAccess(const std::string& nodeId) {
  recordAccess(nodeId, boundSnapshotVersion_);
}

void HotSlice::recordAccess(const std::string& nodeId,
                            const std::uint64_t currentVersion) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  recordAccessLocked(nodeId, currentVersion);
}

void HotSlice::recordAccessLocked(const std::string& nodeId,
                                  const std::uint64_t currentVersion) {
  const std::uint64_t effective = effectiveVersion(currentVersion);
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end()) {
    if (eraseIfStale(it, effective)) {
      return;
    }
    it->second.accessCount++;
    it->second.relevanceScore += 0.1;
    if (effective != 0ULL) {
      it->second.versionStamp = effective;
    }
  }
}

void HotSlice::promote(const std::string& nodeId, double boost) {
  promote(nodeId, boost, boundSnapshotVersion_);
}

void HotSlice::promote(const std::string& nodeId,
                       const double boost,
                       const std::uint64_t currentVersion) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  promoteLocked(nodeId, boost, currentVersion);
}

void HotSlice::promoteLocked(const std::string& nodeId,
                             const double boost,
                             const std::uint64_t currentVersion) {
  const std::uint64_t effective = effectiveVersion(currentVersion);
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end()) {
    if (eraseIfStale(it, effective)) {
      return;
    }
    it->second.relevanceScore += boost;
    if (effective != 0ULL) {
      it->second.versionStamp = effective;
    }
  }
}

void HotSlice::demote(const std::string& nodeId, double penalty) {
  demote(nodeId, penalty, boundSnapshotVersion_);
}

void HotSlice::demote(const std::string& nodeId,
                      const double penalty,
                      const std::uint64_t currentVersion) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  demoteLocked(nodeId, penalty, currentVersion);
}

void HotSlice::demoteLocked(const std::string& nodeId,
                            const double penalty,
                            const std::uint64_t currentVersion) {
  const std::uint64_t effective = effectiveVersion(currentVersion);
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end()) {
    if (eraseIfStale(it, effective)) {
      return;
    }
    it->second.relevanceScore -= penalty;
    if (it->second.relevanceScore < 0.0) {
      it->second.relevanceScore = 0.0;
    }
    if (effective != 0ULL) {
      it->second.versionStamp = effective;
    }
  }
}

// ---------------------------------------------------------------------------
// Store / get / contains
// ---------------------------------------------------------------------------

void HotSlice::storeNode(const StateNode& node) {
  storeNode(node, boundSnapshotVersion_);
}

void HotSlice::storeNode(const StateNode& node, const std::uint64_t versionStamp) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  storeNodeLocked(node, versionStamp);
}

void HotSlice::storeNodeLocked(const StateNode& node,
                               const std::uint64_t versionStamp) {
  const std::uint64_t effective = effectiveVersion(versionStamp);
  auto it = nodes_.find(node.nodeId);
  if (it != nodes_.end()) {
    it->second.data = node;
    it->second.versionStamp =
        effective == 0ULL ? it->second.versionStamp : effective;
    recordAccessLocked(node.nodeId, effective);
  } else {
    TrackedNode tn;
    tn.data = node;
    tn.accessCount = 1;
    tn.relevanceScore = 1.0;
    tn.versionStamp = effective;
    nodes_[node.nodeId] = std::move(tn);
  }

  while (nodes_.size() > maxSize_) {
    evictLowestRelevanceLocked();
  }
  assert(nodes_.size() <= maxSize_);
}

StateNode HotSlice::getNode(const std::string& nodeId) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  lookupCount_.fetch_add(1U, std::memory_order_relaxed);
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end() &&
      (boundSnapshotVersion_ == 0ULL ||
       it->second.versionStamp >= boundSnapshotVersion_)) {
    hitCount_.fetch_add(1U, std::memory_order_relaxed);
    return it->second.data;
  }
  return StateNode{};
}

StateNode HotSlice::getNode(const std::string& nodeId,
                            const std::uint64_t currentVersion) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const std::uint64_t effective = effectiveVersion(currentVersion);
  lookupCount_.fetch_add(1U, std::memory_order_relaxed);
  const auto it = nodes_.find(nodeId);
  if (it == nodes_.end()) {
    return StateNode{};
  }
  if (eraseIfStale(it, effective)) {
    return StateNode{};
  }
  hitCount_.fetch_add(1U, std::memory_order_relaxed);
  nodes_.at(nodeId).accessCount++;
  nodes_.at(nodeId).relevanceScore += 0.1;
  if (effective != 0ULL) {
    nodes_.at(nodeId).versionStamp = effective;
  }
  return nodes_.at(nodeId).data;
}

bool HotSlice::containsNode(const std::string& nodeId) const {
  return containsNode(nodeId, boundSnapshotVersion_);
}

bool HotSlice::containsNode(const std::string& nodeId,
                            const std::uint64_t currentVersion) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const std::uint64_t effective = effectiveVersion(currentVersion);
  const auto it = nodes_.find(nodeId);
  if (it == nodes_.end()) {
    return false;
  }
  return effective == 0ULL || it->second.versionStamp >= effective;
}

// ---------------------------------------------------------------------------
// Top-K queries
// ---------------------------------------------------------------------------

std::vector<StateNode> HotSlice::getTopK(std::size_t k) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<const TrackedNode*> temp;
  temp.reserve(nodes_.size());
  for (const auto& [id, tn] : nodes_) {
    temp.push_back(&tn);
  }
  std::sort(temp.begin(), temp.end(), [](const TrackedNode* a, const TrackedNode* b) {
    if (a->relevanceScore != b->relevanceScore) {
      return a->relevanceScore > b->relevanceScore;
    }
    if (a->accessCount != b->accessCount) {
      return a->accessCount > b->accessCount;
    }
    return a->data.nodeId < b->data.nodeId;
  });
  std::vector<StateNode> result;
  const std::size_t limit = std::min(k, temp.size());
  result.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    result.push_back(temp[i]->data);
  }
  return result;
}

std::vector<StateNode> HotSlice::getTopK(std::size_t k,
                                         const std::uint64_t currentVersion) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const std::uint64_t effective = effectiveVersion(currentVersion);
  for (auto it = nodes_.begin(); it != nodes_.end();) {
    if (effective != 0ULL && it->second.versionStamp < effective) {
      it = nodes_.erase(it);
      continue;
    }
    ++it;
  }
  std::vector<const TrackedNode*> temp;
  temp.reserve(nodes_.size());
  for (const auto& [id, tn] : nodes_) {
    (void)id;
    temp.push_back(&tn);
  }
  std::sort(temp.begin(), temp.end(),
            [](const TrackedNode* a, const TrackedNode* b) {
              if (a->relevanceScore != b->relevanceScore) {
                return a->relevanceScore > b->relevanceScore;
              }
              if (a->accessCount != b->accessCount) {
                return a->accessCount > b->accessCount;
              }
              return a->data.nodeId < b->data.nodeId;
            });
  std::vector<StateNode> result;
  const std::size_t limit = std::min(k, temp.size());
  result.reserve(limit);
  for (std::size_t index = 0U; index < limit; ++index) {
    result.push_back(temp[index]->data);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Version management
// ---------------------------------------------------------------------------

void HotSlice::syncVersions(const std::uint64_t versionStamp) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  boundSnapshotVersion_ = versionStamp;
  for (auto& [id, node] : nodes_) {
    (void)id;
    node.versionStamp = versionStamp;
  }
}

void HotSlice::markStale(const std::string& nodeId,
                         const std::uint64_t currentVersion) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const std::uint64_t effective = effectiveVersion(currentVersion);
  const auto it = nodes_.find(nodeId);
  if (it == nodes_.end()) {
    return;
  }
  if (effective == 0ULL) {
    it->second.versionStamp = 0ULL;
    return;
  }
  it->second.versionStamp = effective - 1ULL;
}

bool HotSlice::eraseIfStale(std::map<std::string, TrackedNode>::iterator it,
                            const std::uint64_t currentVersion) {
  const std::uint64_t effective = effectiveVersion(currentVersion);
  if (it == nodes_.end()) {
    return false;
  }
  if (effective != 0ULL && it->second.versionStamp < effective) {
    nodes_.erase(it);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Eviction
// ---------------------------------------------------------------------------

void HotSlice::evictLowestRelevance() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  evictLowestRelevanceLocked();
}

void HotSlice::evictLowestRelevanceLocked() {
  if (nodes_.empty()) {
    return;
  }
  const auto centralityOf = [](const TrackedNode& tracked) {
    if (!tracked.data.data.is_object()) {
      return 0.0;
    }
    if (!tracked.data.data.contains("centrality") ||
        !tracked.data.data["centrality"].is_number()) {
      return 0.0;
    }
    return clampFinite(tracked.data.data["centrality"].get<double>(), 0.0, 1.0);
  };
  std::vector<std::pair<std::string, const TrackedNode*>> ranked;
  ranked.reserve(nodes_.size());
  for (const auto& [id, tracked] : nodes_) {
    ranked.push_back({id, &tracked});
  }
  std::sort(ranked.begin(), ranked.end(),
            [&centralityOf](const auto& left, const auto& right) {
              if (left.second->relevanceScore != right.second->relevanceScore) {
                return left.second->relevanceScore < right.second->relevanceScore;
              }
              const double leftCentrality = centralityOf(*left.second);
              const double rightCentrality = centralityOf(*right.second);
              if (leftCentrality != rightCentrality) {
                return leftCentrality < rightCentrality;
              }
              if (left.second->accessCount != right.second->accessCount) {
                return left.second->accessCount < right.second->accessCount;
              }
              return left.first > right.first;
            });
  nodes_.erase(ranked.front().first);
  ++evictionCount_;
}

// ---------------------------------------------------------------------------
// Trim
// ---------------------------------------------------------------------------

void HotSlice::trim() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  trimLocked();
}

void HotSlice::trimLocked() {
  if (nodes_.size() <= maxSize_) return;
  const auto centralityOf = [](const TrackedNode& tracked) {
    if (!tracked.data.data.is_object()) {
      return 0.0;
    }
    if (!tracked.data.data.contains("centrality") ||
        !tracked.data.data["centrality"].is_number()) {
      return 0.0;
    }
    return clampFinite(tracked.data.data["centrality"].get<double>(), 0.0, 1.0);
  };
  std::vector<std::pair<std::string, const TrackedNode*>> ranked;
  ranked.reserve(nodes_.size());
  for (const auto& [id, tracked] : nodes_) {
    ranked.push_back({id, &tracked});
  }
  std::sort(ranked.begin(), ranked.end(),
            [&centralityOf](const auto& left, const auto& right) {
              if (left.second->relevanceScore != right.second->relevanceScore) {
                return left.second->relevanceScore < right.second->relevanceScore;
              }
              const double leftCentrality = centralityOf(*left.second);
              const double rightCentrality = centralityOf(*right.second);
              if (leftCentrality != rightCentrality) {
                return leftCentrality < rightCentrality;
              }
              if (left.second->accessCount != right.second->accessCount) {
                return left.second->accessCount < right.second->accessCount;
              }
              return left.first > right.first;
            });
  const std::size_t toRemove = nodes_.size() - maxSize_;
  for (std::size_t i = 0; i < toRemove; ++i) {
    nodes_.erase(ranked[i].first);
    ++evictionCount_;
  }
  assert(nodes_.size() <= maxSize_);
}

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

void HotSlice::calibrate(const ultra::calibration::UsageTracker& tracker) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto history = tracker.getHistory();
  if (history.empty()) return;
  for (auto& [id, tn] : nodes_) {
    tn.relevanceScore *= 0.95;
  }
  for (const auto& event : history) {
    if (event.command == "diff" || event.command == "analyze" ||
        event.command == "build") {
      for (const auto& arg : event.args) {
        promoteLocked(arg, 0.5, boundSnapshotVersion_);
      }
    }
  }
}

void HotSlice::recalibrateDeterministically(const double pruneThreshold) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const double clampedThreshold = clampFinite(pruneThreshold, 0.0, 1.0);
  const auto centralityOf = [](const TrackedNode& tracked) {
    if (!tracked.data.data.is_object()) {
      return 0.0;
    }
    if (!tracked.data.data.contains("centrality") ||
        !tracked.data.data["centrality"].is_number()) {
      return 0.0;
    }
    return clampFinite(tracked.data.data["centrality"].get<double>(), 0.0, 1.0);
  };
  std::size_t maxAccessCount = 1U;
  for (const auto& [id, tracked] : nodes_) {
    (void)id;
    maxAccessCount = std::max(maxAccessCount, tracked.accessCount);
  }
  for (auto& [id, tracked] : nodes_) {
    (void)id;
    const double accessScore =
        static_cast<double>(tracked.accessCount) /
        static_cast<double>(maxAccessCount);
    const double centrality = centralityOf(tracked);
    tracked.relevanceScore = clampFinite(
        (tracked.relevanceScore * 0.85) + (accessScore * 0.10) +
            (centrality * 0.05) - (clampedThreshold * 0.05),
        0.0, std::numeric_limits<double>::max());
  }
  ++recalibrationCount_;
  trimLocked();
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

void HotSlice::bindToSnapshotVersion(const std::uint64_t snapshotVersion) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  boundSnapshotVersion_ = snapshotVersion;
}

void HotSlice::setMaxSize(const std::size_t maxSize) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  maxSize_ = std::max<std::size_t>(1U, maxSize);
  trimLocked();
}

HotSlice::GovernanceStats HotSlice::stats() const noexcept {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  GovernanceStats out;
  out.currentSize = nodes_.size();
  out.maxSize = maxSize_;
  out.lookupCount = lookupCount_.load(std::memory_order_relaxed);
  out.hitCount = hitCount_.load(std::memory_order_relaxed);
  out.evictionCount = evictionCount_;
  out.recalibrationCount = recalibrationCount_;
  const std::size_t lookups = out.lookupCount;
  out.hitRate = lookups == 0U
                    ? 0.0
                    : static_cast<double>(out.hitCount) /
                          static_cast<double>(lookups);
  return out;
}

std::uint64_t HotSlice::effectiveVersion(const std::uint64_t currentVersion) const {
  return currentVersion == 0ULL ? boundSnapshotVersion_ : currentVersion;
}

}  // namespace ultra::memory
