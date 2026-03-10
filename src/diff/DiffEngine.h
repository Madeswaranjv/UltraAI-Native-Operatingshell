#pragma once

#include "BranchSemanticDiff.h"
#include "DeltaReport.h"

#include <external/json.hpp>

#include "../ai/SymbolTable.h"
#include "../graph/DependencyGraph.h"
#include "../memory/StateSnapshot.h"
//DiffEngine.h
#include <cstdint>
#include <vector>

namespace ultra::memory {
class SemanticMemory;
}

namespace ultra::diff {

/// Central facade for the cross-branch semantic diff engine.
class DiffEngine {
 public:
  /// Given an original structural snapshot and a new modified snapshot,
  /// plus the current dependency graph, computes the complete DeltaReport.
  static DeltaReport computeDelta(
      const std::vector<ultra::ai::SymbolRecord>& stateT1,
      const std::vector<ultra::ai::SymbolRecord>& stateT2,
      const ultra::graph::DependencyGraph& depGraph,
      ultra::memory::SemanticMemory* semanticMemory = nullptr,
      std::uint64_t semanticVersion = 0U);

  /// Deterministic cross-branch structural diff from two immutable snapshots.
  static semantic::BranchDiffReport diffBranches(
      const ultra::memory::StateSnapshot& branchA,
      const ultra::memory::StateSnapshot& branchB);

  static nlohmann::ordered_json branchDiffToJson(
      const semantic::BranchDiffReport& report);

  /// Deterministic JSON envelope for CLI and automation callers.
  static nlohmann::ordered_json diffBranchesJson(
      const ultra::memory::StateSnapshot& branchA,
      const ultra::memory::StateSnapshot& branchB);
};

}  // namespace ultra::diff
