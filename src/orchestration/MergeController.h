#pragma once

#include "../intelligence/BranchStore.h"
#include <external/json.hpp>
#include <string>
#include <vector>

namespace ultra::orchestration {

/// Strategy options for handling divergent outputs from parallel reasoning branches.
enum class ConflictResolutionStrategy : std::uint8_t {
  FirstWins = 0,
  HighestConfidence = 1,
  MergeStrict = 2,
  FailOnConflict = 3
};

/// Represents a conflicted field across multiple branches.
struct Conflict {
  std::string fieldPath;
  std::vector<std::string> branchIds;
  std::vector<std::string> conflictingValues;
};

/// Represents the final computed output of merging multiple reasoning branches.
struct ConsolidatedResult {
  nlohmann::json mergedOutput;
  ultra::types::Confidence aggregatedConfidence;
  std::vector<Conflict> conflicts;
  std::string strategyUsed;
  bool success{false};
};

/// Orchestrates the merging of outputs from multiple parallel reasoning sub-branches.
class MergeController {
 public:
  explicit MergeController(ultra::intelligence::BranchStore& store);

  /// Consolidates execution nodes' outputs from the provided branches into a single result.
  ConsolidatedResult consolidate(const std::vector<std::string>& branchIds,
                                 ConflictResolutionStrategy strategy = ConflictResolutionStrategy::HighestConfidence);

 private:
  ultra::intelligence::BranchStore& store_;

  // A deeply nested merge is complex, we stub a basic highest-confidence flat merge for now.
  void applyHighestConfidenceMerge(const std::vector<ultra::intelligence::Branch>& branches, ConsolidatedResult& result);
};

}  // namespace ultra::orchestration
