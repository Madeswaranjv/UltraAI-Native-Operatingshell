#include "MergeController.h"
#include "../core/Logger.h"

namespace ultra::orchestration {

MergeController::MergeController(ultra::intelligence::BranchStore& store) : store_(store) {}

ConsolidatedResult MergeController::consolidate(const std::vector<std::string>& branchIds,
                                                ConflictResolutionStrategy strategy) {
  ConsolidatedResult result;
  std::vector<ultra::intelligence::Branch> branches;
  
  for (const std::string& id : branchIds) {
    auto b = store_.get(id);
    if (!b.branchId.empty()) {
      branches.push_back(b);
    }
  }

  if (branches.empty()) {
    result.success = false;
    return result;
  }
  
  // Aggregate confidence using an arithmetic mean for simplicity
  double totalStability = 0.0;
  for (const auto& b : branches) {
    totalStability += b.confidence.stabilityScore;
  }
  
  result.aggregatedConfidence.stabilityScore = totalStability / branches.size();
  
  // For the actual output payload, we would iterate ExecutionNodes attached to the branch.
  // We'll stub the payload as an array of JSON objects representing branch IDs merged.
  
  if (strategy == ConflictResolutionStrategy::HighestConfidence) {
    applyHighestConfidenceMerge(branches, result);
    result.strategyUsed = "HighestConfidence";
  } else {
    // Other strategies are stubbed for Phase 4
    result.strategyUsed = "Fallback";
  }

  result.success = result.conflicts.empty();
  return result;
}

void MergeController::applyHighestConfidenceMerge(const std::vector<ultra::intelligence::Branch>& branches, 
                                                ConsolidatedResult& result) {
  // Find the branch with the highest confidence
  const ultra::intelligence::Branch* topBranch = nullptr;
  for (const auto& b : branches) {
    if (!topBranch || b.confidence.stabilityScore > topBranch->confidence.stabilityScore) {
      topBranch = &b;
    }
  }

  result.mergedOutput = nlohmann::json::object();
  if (topBranch) {
    result.mergedOutput["winning_branch"] = topBranch->branchId;
    result.mergedOutput["confidence_score"] = topBranch->confidence.stabilityScore;
    result.mergedOutput["goal"] = topBranch->goal;
  }
}

}  // namespace ultra::orchestration
