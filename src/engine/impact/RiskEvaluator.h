#pragma once

#include "ImpactTypes.h"

#include <string>
#include <vector>

namespace ultra::engine::impact {

class RiskEvaluator {
 public:
  [[nodiscard]] RiskAssessment evaluate(
      const ImpactPlan& plan,
      const std::vector<ImpactedFile>& files,
      const std::vector<ImpactedSymbol>& symbols) const;

 private:
  static std::string moduleOf(const std::string& path);
};

}  // namespace ultra::engine::impact
