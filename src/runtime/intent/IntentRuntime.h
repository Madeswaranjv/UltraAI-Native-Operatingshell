#pragma once

#include "Intent.h"
#include "IntentEvaluator.h"
#include "../../orchestration/IntentDecomposer.h"

#include <cstddef>
#include <string>

namespace ultra::runtime::intent {

struct ContextFrame {
  std::string goal;
  std::string target;
  std::string branchId;
  std::size_t tokenBudget{4096U};
  std::size_t impactDepth{2U};
  std::size_t maxFilesChanged{8U};
  RiskTolerance tolerance{RiskTolerance::MEDIUM};
  bool allowPublicApiChange{false};
  double riskThreshold{0.66};
};

class IntentRuntime {
 public:
  [[nodiscard]] Intent process_input(const std::string& input) const;

  [[nodiscard]] Intent resolve_structured_intent(
      const std::string& input,
      const ContextFrame& frame) const;

  [[nodiscard]] ::ultra::orchestration::TaskGraph decompose_intent(
      const Intent& intent) const;

  [[nodiscard]] IntentEvaluation evaluate_intent(
      const Intent& intent,
      const CognitiveState& state) const;

 private:
  [[maybe_unused]] ::ultra::orchestration::IntentDecomposer decomposer_{};
  [[maybe_unused]] IntentEvaluator evaluator_{};
};

}  // namespace ultra::runtime::intent

