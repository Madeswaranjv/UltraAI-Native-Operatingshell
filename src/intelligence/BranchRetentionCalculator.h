#pragma once
// BranchRetentionCalculator.h

#include <string>

namespace ultra::intelligence {

struct Branch;

class BranchRetentionCalculator {
 public:
  // Compute a deterministic retention score for a branch. Lower => more likely to be evicted.
  double computeRetentionScore(const Branch& b, size_t lruRank, size_t totalBranches) const noexcept;
};

}  // namespace ultra::intelligence
