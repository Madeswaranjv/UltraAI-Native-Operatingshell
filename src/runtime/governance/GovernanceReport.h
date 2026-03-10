#pragma once

#include "../intent/PlanScore.h"

#include <string>
#include <vector>

namespace ultra::runtime::governance {

struct GovernanceReport {
  bool approved{false};
  std::string reason;
  intent::RiskScore risk;
  intent::ImpactScore impact;
  intent::TokenCostEstimate tokenCost;
  intent::DeterminismScore determinism;
  std::vector<std::string> violations;
};

struct GovernedStrategyResult {
  intent::PlanScore plan;
  GovernanceReport governance;
};

}  // namespace ultra::runtime::governance

