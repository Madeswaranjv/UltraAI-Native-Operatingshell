#pragma once

#include "../intent/Intent.h"

namespace ultra::runtime::governance {

struct Policy {
  intent::RiskTolerance maxRisk{intent::RiskTolerance::MEDIUM};
  int maxFilesChanged{8};
  int maxImpactDepth{2};
  int maxTokenBudget{4096};
  bool allowPublicAPIChange{false};
  bool requireDeterminism{true};
  bool allowCrossModuleMove{false};
};

[[nodiscard]] Policy normalizePolicy(const Policy& policy, int fallbackTokenBudget);

}  // namespace ultra::runtime::governance

