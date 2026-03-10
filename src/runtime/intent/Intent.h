#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ultra::runtime::intent {

enum class GoalType : std::uint8_t {
  ModifySymbol = 0,
  RefactorModule = 1,
  ReduceImpactRadius = 2,
  ImproveCentrality = 3,
  MinimizeTokenUsage = 4,
  AddDependency = 5,
  RemoveDependency = 6
};

struct Goal {
  GoalType type{GoalType::ModifySymbol};
  std::string target;
};

struct Constraints {
  std::size_t maxImpactDepth{2U};
  std::size_t maxFilesChanged{8U};
  std::size_t tokenBudget{0U};
  std::string branchScope;
  bool determinismRequired{true};
};

enum class RiskTolerance : std::uint8_t {
  LOW = 0,
  MEDIUM = 1,
  HIGH = 2
};

struct StrategyOptions {
  bool allowRename{false};
  bool allowSignatureChange{false};
  bool allowPublicAPIChange{false};
  bool allowCrossModuleMove{false};
};

struct Intent {
  Goal goal;
  Constraints constraints;
  RiskTolerance risk{RiskTolerance::MEDIUM};
  StrategyOptions options;
};

[[nodiscard]] std::string toString(GoalType type);
[[nodiscard]] std::string toString(RiskTolerance risk);
[[nodiscard]] std::size_t riskRank(RiskTolerance risk);
[[nodiscard]] Intent normalizeIntent(const Intent& intent,
                                     std::size_t fallbackTokenBudget);

}  // namespace ultra::runtime::intent

