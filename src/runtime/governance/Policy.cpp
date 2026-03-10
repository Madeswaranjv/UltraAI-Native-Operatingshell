#include "Policy.h"

#include <algorithm>

namespace ultra::runtime::governance {

Policy normalizePolicy(const Policy& policy, const int fallbackTokenBudget) {
  Policy normalized = policy;
  normalized.maxFilesChanged = std::max(1, normalized.maxFilesChanged);
  normalized.maxImpactDepth = std::max(1, normalized.maxImpactDepth);
  if (normalized.maxTokenBudget <= 0) {
    normalized.maxTokenBudget = std::max(1, fallbackTokenBudget);
  }
  return normalized;
}

}  // namespace ultra::runtime::governance

