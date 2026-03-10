#pragma once
//intelligence\BranchLifecycle.h
#include "Branch.h"
#include "BranchEvictionPolicy.h"
#include "BranchStore.h"
#include "../memory/SnapshotChain.h"
#include "../memory/StateGraph.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ultra::intelligence {

/// Orchestrates state transitions for AI reasoning branches.
class BranchLifecycle {
 public:
  explicit BranchLifecycle(BranchStore& store);
  BranchLifecycle(BranchStore& store, ultra::memory::SnapshotChain& chain, ultra::memory::StateGraph& activeGraph);

  /// Create a new root branch with no parent.
  Branch create(const std::string& goal);

  /// Spawn a new child branch from a parent to pursue a sub-goal.
  /// Creates a memory snapshot frozen at the time of spawning.
  Branch spawn(const std::string& parentId, const std::string& goal);

  /// Persist the main execution state and mark branch as Suspended.
  bool suspend(const std::string& branchId);

  /// Mark branch as Active and restore its frozen execution context.
  bool resume(const std::string& branchId);

  /// Attempt to merge the findings/outputs of the source branch into the target branch.
  bool merge(const std::string& sourceId, const std::string& targetId);

  /// Permanently freeze a branch and mark as Archived.
  bool archive(const std::string& branchId);

  /// Erase all memory mutations caused by this branch, restoring the graph
  /// to the exact snapshot recorded when this branch was spawned.
  bool rollback(const std::string& branchId);

 private:
  static constexpr std::size_t kMaxActiveBranches = 3U;
  static constexpr std::size_t kMaxTrackedBranches = 12U;

  void runMemoryGovernance(const std::string& protectedBranchId,
                           bool heavyMutation);
  void enforceActiveBranchCap(const std::string& protectedBranchId);
  void invalidateStaleSnapshotReferences();
  bool evictBranch(const std::string& branchId);

  BranchStore& store_;
  ultra::memory::SnapshotChain* chain_{nullptr};
  ultra::memory::StateGraph* activeGraph_{nullptr};
  std::unique_ptr<ultra::memory::SnapshotChain> ownedChain_;
  std::unique_ptr<ultra::memory::StateGraph> ownedGraph_;
  BranchEvictionPolicy evictionPolicy_;
  std::string activeBranchId_;
  std::size_t evictionCount_{0U};
};

}  // namespace ultra::intelligence
