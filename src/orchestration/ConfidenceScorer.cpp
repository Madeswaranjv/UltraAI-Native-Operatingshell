#include "ConfidenceScorer.h"
#include <algorithm>

namespace ultra::orchestration {

ConfidenceScorer::ConfidenceScorer(
    ultra::intelligence::BranchStore& store)
    : store_(store) {}

ultra::types::Confidence
ConfidenceScorer::computeBaseScore(
    const ultra::intelligence::Branch& branch) const {

  ultra::types::Confidence conf;

  using State = ultra::intelligence::BranchState;

  switch (branch.status) {

    case State::Archived:
      conf.stabilityScore = 1.0;
      conf.riskAdjustedConfidence = 0.95;
      conf.decisionReliabilityIndex = 0.98;
      break;

    case State::Active:
      conf.stabilityScore = 0.8;
      conf.riskAdjustedConfidence = 0.7;
      conf.decisionReliabilityIndex = 0.75;
      break;

    case State::Suspended:
      conf.stabilityScore = 0.5;
      conf.riskAdjustedConfidence = 0.4;
      conf.decisionReliabilityIndex = 0.3;
      break;

    case State::RolledBack:
      conf.stabilityScore = 0.4;
      conf.riskAdjustedConfidence = 0.3;
      conf.decisionReliabilityIndex = 0.2;
      break;

    case State::Merged:
      conf.stabilityScore = 0.6;
      conf.riskAdjustedConfidence = 0.5;
      conf.decisionReliabilityIndex = 0.5;
      break;

    default:
      conf.stabilityScore = 0.0;
      conf.riskAdjustedConfidence = 0.0;
      conf.decisionReliabilityIndex = 0.0;
      break;
  }

  // Memory penalty
  if (branch.memorySnapshotId.empty()) {
    conf.stabilityScore *= 0.8;
    conf.riskAdjustedConfidence *= 0.6;
    conf.decisionReliabilityIndex *= 0.5;
  }

  // Clamp
  conf.stabilityScore =
      std::clamp(conf.stabilityScore, 0.0, 1.0);
  conf.riskAdjustedConfidence =
      std::clamp(conf.riskAdjustedConfidence, 0.0, 1.0);
  conf.decisionReliabilityIndex =
      std::clamp(conf.decisionReliabilityIndex, 0.0, 1.0);

  return conf;
}

ultra::types::Confidence
ConfidenceScorer::score(const std::string& branchId) const {

  auto branch = store_.get(branchId);
  if (branch.branchId.empty())
    return ultra::types::Confidence{};

  auto base = computeBaseScore(branch);

  if (!branch.subBranches.empty()) {
    auto sub = scoreConsolidated(branch.subBranches);

    // 60/40 weighting (expected by earlier design)
    base.stabilityScore =
        (base.stabilityScore * 0.6) +
        (sub.stabilityScore * 0.4);

    base.riskAdjustedConfidence =
        (base.riskAdjustedConfidence * 0.6) +
        (sub.riskAdjustedConfidence * 0.4);

    base.decisionReliabilityIndex =
        (base.decisionReliabilityIndex * 0.6) +
        (sub.decisionReliabilityIndex * 0.4);
  }

  return base;
}

ultra::types::Confidence
ConfidenceScorer::scoreConsolidated(
    const std::vector<std::string>& branchIds) const {

  ultra::types::Confidence agg;

  if (branchIds.empty())
    return agg;

  double stability = 0.0;
  double risk = 0.0;
  double reliability = 0.0;
  int count = 0;

  for (const auto& id : branchIds) {
    auto b = store_.get(id);
    if (!b.branchId.empty()) {
      auto c = computeBaseScore(b);
      stability += c.stabilityScore;
      risk += c.riskAdjustedConfidence;
      reliability += c.decisionReliabilityIndex;
      count++;
    }
  }

  if (count > 0) {
    agg.stabilityScore = stability / count;
    agg.riskAdjustedConfidence = risk / count;
    agg.decisionReliabilityIndex = reliability / count;
  }

  return agg;
}

}  // namespace ultra::orchestration