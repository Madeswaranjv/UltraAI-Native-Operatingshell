#pragma once

#include "Strategy.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ultra::runtime::intent {

enum class ExecutionMode : std::uint8_t {
  Reject = 0U,
  Simulate = 1U,
  Direct = 2U
};

struct PlanScore {
  std::size_t rank{0U};
  Strategy strategy;
  double score{0.0};
  RiskTolerance riskClassification{RiskTolerance::LOW};
  std::size_t estimatedTokenUsage{0U};
  double estimatedImpactRadius{0.0};
  double determinismScore{1.0};
  ExecutionMode executionMode{ExecutionMode::Reject};
  bool accepted{false};
  bool usesImpactPrediction{false};
  std::string decisionReason;
};

[[nodiscard]] inline std::string toString(const ExecutionMode mode) {
  switch (mode) {
    case ExecutionMode::Reject:
      return "Reject";
    case ExecutionMode::Simulate:
      return "Simulate";
    case ExecutionMode::Direct:
      return "Direct";
  }
  return "Reject";
}

}  // namespace ultra::runtime::intent
