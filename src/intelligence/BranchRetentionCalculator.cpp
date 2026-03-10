#include "BranchRetentionCalculator.h"
#include "Branch.h"
//E:\Projects\Ultra\src\intelligence\BranchRetentionCalculator.cpp
#include <algorithm>

namespace ultra::intelligence {

double BranchRetentionCalculator::computeRetentionScore(const Branch& b, size_t lruRank, size_t totalBranches) const noexcept {
  // LRU_Age_Factor: more recently used -> closer to 1.0, older -> closer to 0.0
  double lruFactor = 1.0;
  if (totalBranches > 0) {
    lruFactor = 1.0 - (static_cast<double>(lruRank) / static_cast<double>(totalBranches));
    if (lruFactor < 0.0) lruFactor = 0.0;
  }

  // Deterministic usage proxies:
  // query frequency from LRU rank, edit frequency from mutation span,
  // impact traversal usage from dependency references and in-flight execution.
  const double queryFrequency =
      1.0 + (lruFactor * 0.75);
  const double editFrequency =
      1.0 + std::min<double>(
                static_cast<double>(
                    b.lastMutationSequence > b.creationSequence
                        ? (b.lastMutationSequence - b.creationSequence)
                        : 0ULL),
                16.0) *
                0.04;
  const double impactTraversalUsage =
      1.0 + std::min<double>(
                static_cast<double>(b.dependencyReferences.size()), 16.0) *
                0.05 +
      (!b.currentExecutionNodeId.empty() ? 0.25 : 0.0);

  double stabilityComponent =
      0.75 + std::clamp(b.confidence.stabilityScore, 0.0, 1.0) * 0.50;
  double decisionComponent =
      0.75 +
      std::clamp(b.confidence.decisionReliabilityIndex, 0.0, 1.0) * 0.50;

  // RiskFactor (higher risk increases retention slightly)
  double riskFactor = 1.0 + (1.0 - std::clamp(b.confidence.riskAdjustedConfidence, 0.0, 1.0)) * 0.5;

  // State multiplier
  double stateMultiplier = 1.0;
  switch (b.status) {
    case BranchState::Active: stateMultiplier = 10.0; break;
    case BranchState::Suspended: stateMultiplier = 1.0; break;
    case BranchState::Archived: stateMultiplier = 0.5; break;
    case BranchState::Merged: stateMultiplier = 0.3; break;
    case BranchState::RolledBack: stateMultiplier = 0.2; break;
    default: stateMultiplier = 1.0; break;
  }

  double score = lruFactor * queryFrequency * editFrequency *
                 impactTraversalUsage * stabilityComponent *
                 decisionComponent * riskFactor * stateMultiplier;
  return score;
}

}  // namespace ultra::intelligence
