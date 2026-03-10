#pragma once

#include <cstddef>

namespace ultra::diff::semantic {
struct BranchDiffReport;
}  // namespace ultra::diff::semantic

namespace ultra::authority {

struct AuthorityRiskReport;

class RiskEvaluator {
 public:
  [[nodiscard]] AuthorityRiskReport evaluate(
      const diff::semantic::BranchDiffReport& diffReport,
      std::size_t impactDepth,
      std::size_t publicApiChanges,
      double threshold) const;
};

}  // namespace ultra::authority
