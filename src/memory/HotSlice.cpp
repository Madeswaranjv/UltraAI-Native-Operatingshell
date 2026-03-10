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

HotSlice::HotSlice(std::size_t maxSize)
    : maxSize_(std::max<std::size_t>(1U, maxSize)) {}

void HotSlice::recordAccess(const std::string& nodeId) {
  recordAccess(nodeId, boundSnapshotVersion_);
}

void HotSlice::recordAccess(const std::string& nodeId,
                            const std::uint64_t currentVersion) {
  const std::uint64_t effective = effectiveVersion(currentVersion);
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end()) {
    if (eraseIfStale(it, effective)) {
      return;
    }
    it->second.accessCount++;
    it->second.relevanceScore += 0.1; // Simple hit boost
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

void HotSlice::storeNode(const StateNode& node) {
  storeNode(node, boundSnapshotVersion_);
}

void HotSlice::storeNode(const StateNode& node, const std::uint64_t versionStamp) {
  const std::uint64_t effective = effectiveVersion(versionStamp);
  auto it = nodes_.find(node.nodeId);
  if (it != nodes_.end()) {
    it->second.data = node;
    it->second.versionStamp =
        effective == 0ULL ? it->second.versionStamp : effective;
    recordAccess(node.nodeId, effective);
  } else {
    TrackedNode tn;
    tn.data = node;
    tn.accessCount = 1;
    tn.relevanceScore = 1.0;
    tn.versionStamp = effective;
    nodes_[node.nodeId] = std::move(tn);
  }
  
  // INVARIANT ENFORCEMENT: Capacity must never be exceeded
  // While size exceeds maxSize, evict lowest relevance items
  while (nodes_.size() > maxSize_) {
    evictLowestRelevance();
  }
  
  // Debug assertion: verify invariant holds
  assert(nodes_.size() <= maxSize_);
}

StateNode HotSlice::getNode(const std::string& nodeId) const {
  ++lookupCount_;
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end() &&
      (boundSnapshotVersion_ == 0ULL ||
       it->second.versionStamp >= boundSnapshotVersion_)) {
    ++hitCount_;
    // Cannot mutate access count in const getter unfortunately,
    // caller should call recordAccess().
    return it->second.data;
  }
  return StateNode{};
}

StateNode HotSlice::getNode(const std::string& nodeId,
                            const std::uint64_t currentVersion) {
  const std::uint64_t effective = effectiveVersion(currentVersion);
  ++lookupCount_;
  const auto it = nodes_.find(nodeId);
  if (it == nodes_.end()) {
    return StateNode{};
  }
  if (eraseIfStale(it, effective)) {
    return StateNode{};
  }
  ++hitCount_;
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
  const std::uint64_t effective = effectiveVersion(currentVersion);
  const auto it = nodes_.find(nodeId);
  if (it == nodes_.end()) {
    return false;
  }
  return effective == 0ULL || it->second.versionStamp >= effective;
}

std::vector<StateNode> HotSlice::getTopK(std::size_t k) const {
  std::vector<const TrackedNode*> temp;
  temp.reserve(nodes_.size());
  for (const auto& [id, tn] : nodes_) {
    temp.push_back(&tn);
  }

  // Sort descending by relevance score
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
  std::size_t limit = std::min(k, temp.size());
  result.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    result.push_back(temp[i]->data);
  }

  return result;
}

std::vector<StateNode> HotSlice::getTopK(std::size_t k,
                                         const std::uint64_t currentVersion) {
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

void HotSlice::syncVersions(const std::uint64_t versionStamp) {
  boundSnapshotVersion_ = versionStamp;
  for (auto& [id, node] : nodes_) {
    (void)id;
    node.versionStamp = versionStamp;
  }
}

void HotSlice::markStale(const std::string& nodeId,
                         const std::uint64_t currentVersion) {
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

void HotSlice::evictLowestRelevance() {
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

void HotSlice::trim() {
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

  // Remove the lowest scoring nodes until we hit maxSize
  std::size_t toRemove = nodes_.size() - maxSize_;
  for (std::size_t i = 0; i < toRemove; ++i) {
    nodes_.erase(ranked[i].first);
    ++evictionCount_;
  }
  
  // INVARIANT ENFORCEMENT: Verify trim brought us back to capacity
  assert(nodes_.size() <= maxSize_);
}

void HotSlice::calibrate(const ultra::calibration::UsageTracker& tracker) {
  auto history = tracker.getHistory();
  if (history.empty()) return;

  // Apply a small decay factor to all current nodes to prevent stale data from lingering forever
  for (auto& [id, tn] : nodes_) {
    tn.relevanceScore *= 0.95;
  }

  // Boost specific arguments detected in the recent operational history
  for (const auto& event : history) {
    if (event.command == "diff" || event.command == "analyze" || event.command == "build") {
      for (const auto& arg : event.args) {
         promote(arg, 0.5); // Provide a relevance boost to nodes named in args
      }
    }
  }
}

void HotSlice::recalibrateDeterministically(const double pruneThreshold) {
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
  trim();
}

void HotSlice::bindToSnapshotVersion(const std::uint64_t snapshotVersion) {
  boundSnapshotVersion_ = snapshotVersion;
}

void HotSlice::setMaxSize(const std::size_t maxSize) {
  maxSize_ = std::max<std::size_t>(1U, maxSize);
  trim();
}

HotSlice::GovernanceStats HotSlice::stats() const noexcept {
  GovernanceStats out;
  out.currentSize = nodes_.size();
  out.maxSize = maxSize_;
  out.lookupCount = lookupCount_;
  out.hitCount = hitCount_;
  out.evictionCount = evictionCount_;
  out.recalibrationCount = recalibrationCount_;
  out.hitRate = lookupCount_ == 0U
                    ? 0.0
                    : static_cast<double>(hitCount_) /
                          static_cast<double>(lookupCount_);
  return out;
}

std::uint64_t HotSlice::effectiveVersion(const std::uint64_t currentVersion) const {
  return currentVersion == 0ULL ? boundSnapshotVersion_ : currentVersion;
}

}  // namespace ultra::memory
