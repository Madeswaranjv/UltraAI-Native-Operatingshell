#pragma once
//intelligence\Branch.h
#include "BranchState.h"
#include "../types/Confidence.h"

#include <string>
#include <vector>
#include <cstdint>

namespace ultra::intelligence {

/// A Branch represents an isolated cognitive process or reasoning trajectory.
/// Revised for deterministic identity and immutable base/overlay semantics.
struct Branch {
  /// Unique identifier of this reasoning branch.
  std::string branchId;

  /// Identifier of the branch from which this originated. Empty if root.
  std::string parentId;

  /// Compatibility alias used by legacy orchestration/test code.
  /// Kept in sync with parentId by store/lifecycle write paths.
  std::string parentBranchId;

  /// The high-level intent or goal of this branch.
  std::string goal;

  /// Reference to the current acting execution node, if any.
  std::string currentExecutionNodeId;

  /// Any sub-branches that were spawned to delegate sub-goals.
  std::vector<std::string> subBranches;

  /// Reference to the memory environment snapshot id this branch operates on.
  std::string memorySnapshotId;

  /// Graph node dependencies this branch cares about.
  std::vector<std::string> dependencyReferences;

  /// AI confidence in the success trajectory of this branch.
  ultra::types::Confidence confidence;

  /// Current operational state of the branch.
  BranchState status{BranchState::Unknown};

  /// True when runtime overlay memory for this branch is resident.
  bool isOverlayResident{true};

  // Deterministic sequencing fields (assigned by BranchStore/Sequence Manager)
  uint64_t creationSequence{0};
  uint64_t lastMutationSequence{0};
};

}  // namespace ultra::intelligence
