#pragma once

#include "../types/Confidence.h"
#include "../intelligence/Branch.h"
#include "../intelligence/BranchStore.h"
#include <vector>

namespace ultra::orchestration {

/// Quantifies the reliability and certainty of reasoning branch outputs.
class ConfidenceScorer {
 public:
  explicit ConfidenceScorer(ultra::intelligence::BranchStore& store);

  /// Computes a confidence score for a single branch.
  ultra::types::Confidence score(const std::string& branchId) const;

  /// Computes an aggregated confidence score across multiple branches.
  ultra::types::Confidence scoreConsolidated(const std::vector<std::string>& branchIds) const;

 private:
  ultra::intelligence::BranchStore& store_;

  // Compute a base score for a branch without sub-branch recursion
  ultra::types::Confidence computeBaseScore(const ultra::intelligence::Branch& branch) const;
};

}  // namespace ultra::orchestration
