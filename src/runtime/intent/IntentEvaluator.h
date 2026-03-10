#pragma once

#include "../CognitiveState.h"
#include "Intent.h"
#include "PlanScore.h"
#include "Strategy.h"
#include "../../orchestration/TaskGraph.h"

#include <string>
#include <vector>

namespace ultra::runtime::intent {

struct IntentEvaluation {
  Intent normalizedIntent;
  orchestration::TaskGraph executionGraph;
  std::vector<std::string> orderedTasks;
  std::vector<Strategy> strategies;
  std::vector<PlanScore> rankedPlans;
  bool hasBestPlan{false};
  PlanScore bestPlan;
};

class IntentEvaluator {
 public:
  [[nodiscard]] std::vector<Strategy> generateStrategies(
      const Intent& intent,
      const CognitiveState& state) const;

  [[nodiscard]] std::vector<PlanScore> evaluateStrategies(
      const std::vector<Strategy>& strategies,
      const CognitiveState& state) const;

  [[nodiscard]] IntentEvaluation evaluateIntent(
      const Intent& intent,
      const CognitiveState& state) const;
};

}  // namespace ultra::runtime::intent
