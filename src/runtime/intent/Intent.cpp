#include "Intent.h"

#include <algorithm>
#include <filesystem>

namespace ultra::runtime::intent {

namespace {

std::string normalizeBranchScope(const std::string& scope) {
  if (scope.empty()) {
    return {};
  }
  std::string normalized = scope;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  normalized = std::filesystem::path(normalized).lexically_normal().generic_string();
  if (normalized == ".") {
    return {};
  }
  if (normalized.size() >= 2U && normalized[0] == '.' && normalized[1] == '/') {
    normalized.erase(0, 2U);
  }
  return normalized;
}

}  // namespace

std::string toString(const GoalType type) {
  switch (type) {
    case GoalType::ModifySymbol:
      return "ModifySymbol";
    case GoalType::RefactorModule:
      return "RefactorModule";
    case GoalType::ReduceImpactRadius:
      return "ReduceImpactRadius";
    case GoalType::ImproveCentrality:
      return "ImproveCentrality";
    case GoalType::MinimizeTokenUsage:
      return "MinimizeTokenUsage";
    case GoalType::AddDependency:
      return "AddDependency";
    case GoalType::RemoveDependency:
      return "RemoveDependency";
  }
  return "ModifySymbol";
}

std::string toString(const RiskTolerance risk) {
  switch (risk) {
    case RiskTolerance::LOW:
      return "LOW";
    case RiskTolerance::MEDIUM:
      return "MEDIUM";
    case RiskTolerance::HIGH:
      return "HIGH";
  }
  return "MEDIUM";
}

std::size_t riskRank(const RiskTolerance risk) {
  switch (risk) {
    case RiskTolerance::LOW:
      return 0U;
    case RiskTolerance::MEDIUM:
      return 1U;
    case RiskTolerance::HIGH:
      return 2U;
  }
  return 1U;
}

Intent normalizeIntent(const Intent& intent, const std::size_t fallbackTokenBudget) {
  Intent normalized = intent;
  normalized.constraints.maxImpactDepth =
      std::max<std::size_t>(1U, normalized.constraints.maxImpactDepth);
  normalized.constraints.maxFilesChanged =
      std::max<std::size_t>(1U, normalized.constraints.maxFilesChanged);
  if (normalized.constraints.tokenBudget == 0U) {
    normalized.constraints.tokenBudget = std::max<std::size_t>(1U, fallbackTokenBudget);
  }
  normalized.constraints.branchScope =
      normalizeBranchScope(normalized.constraints.branchScope);
  return normalized;
}

}  // namespace ultra::runtime::intent

