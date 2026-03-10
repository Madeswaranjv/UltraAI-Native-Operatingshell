//BranchOrchestrator..h
#pragma once

#include "TaskGraph.h"
#include "../intelligence/BranchLifecycle.h"

#include <string>
#include <vector>
#include <set>

namespace ultra::orchestration {

/// Takes a structured TaskGraph and spawns a cognitive branch hierarchy.
/// 
/// GUARANTEES:
/// - Memory safe: no raw pointers, all accesses checked
/// - Deterministic: topologically ordered, with lexicographic tie-breaking
/// - DAG validated: cycles detected before execution
/// - Exception safe: no crashes on invalid input
/// - Always returns a complete vector, never empty iterators or dangling refs
class BranchOrchestrator {
 public:
  explicit BranchOrchestrator(ultra::intelligence::BranchLifecycle& lifecycle);

  /// Orchestrate the execution of a TaskGraph under a parent branch.
  /// 
  /// CONTRACT:
  /// - Returns a vector of spawned branch IDs (one per valid task node)
  /// - Returns empty vector if graph is empty or has a cycle
  /// - Each returned branch ID is valid in the store
  /// - Parent branch ID is properly set on all spawned branches
  /// - Task descriptions are preserved in branch goals
  /// - Completely deterministic: same graph always produces same order
  ///
  /// SAFETY:
  /// - No crash on missing nodes in dependencies
  /// - No crash on cycles (detected upfront)
  /// - No crash on empty graph
  /// - No memory leaks or dangling references
  /// - No iterator invalidation
  std::vector<std::string> orchestrate(const TaskGraph& graph, const std::string& parentBranchId);

 private:
  ultra::intelligence::BranchLifecycle& lifecycle_;

  /// Validate graph has no cycles and all dependencies refer to valid nodes.
  /// Returns true if valid (even if empty), false if cycle detected.
  bool validateGraph(const TaskGraph& graph) const;

  /// Get topologically sorted node IDs with deterministic ordering.
  /// When multiple nodes are ready, sort lexicographically by ID.
  /// Empty graph returns empty vector.
  std::vector<std::string> getDeterministicTopologicalOrder(const TaskGraph& graph) const;

  /// Spawn branches for each task, preserving all metadata.
  /// Takes pre-validated topological order.
  /// Returns vector of spawned branch IDs (never null, always complete).
  std::vector<std::string> spawnBranches(
      const TaskGraph& graph,
      const std::vector<std::string>& order,
      const std::string& parentBranchId);
};

}  // namespace ultra::orchestration
