#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

struct StrategyScore {
  double successRate{0.0};
  std::size_t failureCount{0U};
  double recoveryCost{0.0};
  double executionEfficiency{0.0};
  double confidenceScore{0.0};
};

struct PlanPerformance {
  std::uint64_t planHash{0U};
  double score{0.0};
  std::size_t iterationIndex{0U};
  std::string strategyType;
};

struct StrategyFeedbackMemory {
  std::string strategyType;
  std::string outcome;
  std::uint64_t planHash{0U};
  StrategyScore latestScore{};
  std::vector<PlanPerformance> recentPlans;
  bool reinforcePattern{false};
  bool simplifyPlan{false};
  bool increaseTaskGranularity{false};
  bool avoidRepeatedPlan{false};
  bool forceVariation{false};
  std::string preferredStrategyType;
  std::string avoidedStrategyType;
};

struct IntentMemoryContext {
  std::string queryKey;
  std::vector<std::string> pastSimilarGoals;
  std::vector<std::string> knownConstraints;
  std::vector<std::string> priorOutcomes;
  std::vector<std::string> successfulPatterns;
  std::vector<std::string> failedPatterns;
  std::vector<std::string> recoveryPatterns;
  StrategyFeedbackMemory strategyFeedback{};
  std::vector<PlanPerformance> recentPlanPerformance;
  bool repeatedFailureDetected{false};
  bool hasReusableStrategy{false};
};

struct Intent {
  Goal goal;
  Constraints constraints;
  RiskTolerance risk{RiskTolerance::MEDIUM};
  StrategyOptions options;
  IntentMemoryContext memory;
  std::string rawPrompt;
};

[[nodiscard]] std::string toString(GoalType type);
[[nodiscard]] std::string toString(RiskTolerance risk);
[[nodiscard]] std::size_t riskRank(RiskTolerance risk);
[[nodiscard]] Intent normalizeIntent(const Intent& intent,
                                     std::size_t fallbackTokenBudget);

}  // namespace ultra::runtime::intent
