#pragma once
//E:\Projects\Ultra\src\memory\HotSlice.h
#include "StateGraph.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "../calibration/UsageTracker.h"

namespace ultra::memory {

/// Adaptive high-relevance subgraph cache for fast access.
/// Tracks access frequency and automatically evicts cold nodes.
class HotSlice {
 public:
  // Minimum floor — never shrink below this regardless of graph size.
  static constexpr std::size_t kMinHotSliceEntries = 256U;

  // Backward-compatible alias so all existing callers of kMaxHotSliceEntries
  // continue to compile without changes.
  static constexpr std::size_t kMaxHotSliceEntries = kMinHotSliceEntries;

  // Legacy default kept for zero-arg construction (small repos / tests).
  static constexpr std::size_t kDefaultHotSliceEntries = kMinHotSliceEntries;

  /// Compute the correct hot-slice capacity for a graph of this size.
  /// Formula: max(avgSnapshotNodes * 2, graphNodeCount / 20, kMinHotSliceEntries)
  /// Call once after each index rebuild and pass the result to setMaxSize().
  /// avgSnapshotNodes = 0 means "use graph size only".
  [[nodiscard]] static std::size_t computeCapacity(
      std::size_t graphNodeCount,
      std::size_t avgSnapshotNodes = 0U) noexcept;

  struct GovernanceStats {
    std::size_t currentSize{0U};
    std::size_t maxSize{0U};
    std::size_t lookupCount{0U};
    std::size_t hitCount{0U};
    std::size_t evictionCount{0U};
    std::size_t recalibrationCount{0U};
    double hitRate{0.0};
  };

  explicit HotSlice(std::size_t maxSize = kDefaultHotSliceEntries);

  /// Increment access count for a node, potentially promoting it.
  void recordAccess(const std::string& nodeId);
  void recordAccess(const std::string& nodeId, std::uint64_t currentVersion);

  /// Increase relevance score directly manually via feedback.
  void promote(const std::string& nodeId, double boost = 1.0);
  void promote(const std::string& nodeId,
               double boost,
               std::uint64_t currentVersion);

  /// Decrease relevance via feedback.
  void demote(const std::string& nodeId, double penalty = 1.0);
  void demote(const std::string& nodeId,
              double penalty,
              std::uint64_t currentVersion);

  /// Ensure the node exists in the hot slice (adds if missing).
  void storeNode(const StateNode& node);
  void storeNode(const StateNode& node, std::uint64_t versionStamp);

  /// Fetch a node if it is currently hot.
  StateNode getNode(const std::string& nodeId) const;
  StateNode getNode(const std::string& nodeId, std::uint64_t currentVersion);
  [[nodiscard]] bool containsNode(const std::string& nodeId) const;
  [[nodiscard]] bool containsNode(const std::string& nodeId,
                                  std::uint64_t currentVersion) const;

  /// Get the top K most relevant nodes.
  std::vector<StateNode> getTopK(std::size_t k) const;
  std::vector<StateNode> getTopK(std::size_t k, std::uint64_t currentVersion);

  /// Align all live entries to the current structural version.
  void syncVersions(std::uint64_t versionStamp);

  /// Mark a specific entry stale relative to current version.
  void markStale(const std::string& nodeId, std::uint64_t currentVersion);

  /// Bind this working memory to a specific graph snapshot version.
  void bindToSnapshotVersion(std::uint64_t snapshotVersion);

  [[nodiscard]] std::uint64_t boundSnapshotVersion() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return boundSnapshotVersion_;
  }

  /// Calibrate relevance based on usage tracker history.
  void calibrate(const ultra::calibration::UsageTracker& tracker);
  void recalibrateDeterministically(double pruneThreshold = 0.0);

  /// Trigger an eviction cycle to trim to maxSize.
  void trim();

  /// Evict single lowest-relevance node (used for capacity enforcement).
  void evictLowestRelevance();
  void setMaxSize(std::size_t maxSize);

  std::size_t currentSize() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return nodes_.size();
  }
  [[nodiscard]] std::size_t maxSize() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return maxSize_;
  }
  [[nodiscard]] GovernanceStats stats() const noexcept;

 private:
  void recordAccessLocked(const std::string& nodeId,
                          std::uint64_t currentVersion);
  void promoteLocked(const std::string& nodeId,
                     double boost,
                     std::uint64_t currentVersion);
  void demoteLocked(const std::string& nodeId,
                    double penalty,
                    std::uint64_t currentVersion);
  void storeNodeLocked(const StateNode& node, std::uint64_t versionStamp);
  void evictLowestRelevanceLocked();
  void trimLocked();

  struct TrackedNode {
    StateNode data;
    std::size_t accessCount{1};
    double relevanceScore{1.0};
    std::uint64_t versionStamp{0};
  };

  bool eraseIfStale(std::map<std::string, TrackedNode>::iterator it,
                    std::uint64_t currentVersion);
  std::uint64_t effectiveVersion(std::uint64_t currentVersion) const;

  std::size_t maxSize_;
  std::uint64_t boundSnapshotVersion_{0U};
  std::map<std::string, TrackedNode> nodes_;
  mutable std::atomic<std::size_t> lookupCount_{0U};
  mutable std::atomic<std::size_t> hitCount_{0U};
  std::size_t evictionCount_{0U};
  std::size_t recalibrationCount_{0U};
  mutable std::shared_mutex mutex_;
};

}  // namespace ultra::memory
