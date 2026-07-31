#pragma once

#include "ExecutionKernel.h"
#include "micro_planner.h"
#include "task_graph.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ultra::memory {
class CognitiveMemoryManager;
}

namespace ultra::runtime::cognitive {

enum class UltraLoopState : std::uint8_t {
  INIT = 0U,
  PLAN = 1U,
  ARBITRATION = 2U,
  MICRO_PLAN = 3U,
  EXECUTE = 4U,
  PARTIAL_REPAIR = 5U,
  VERIFY = 6U,
  REFLECT = 7U,
  RE_ANCHOR = 8U,
  REPLAN = 9U,
  TERMINATE = 10U
};

[[nodiscard]] const char* toString(UltraLoopState state) noexcept;

enum class StageSignal : std::uint8_t {
  Continue = 0U,
  Retry = 1U,
  Replan = 2U,
  TerminateSuccess = 3U,
  TerminateFailure = 4U,
  SIG_CONVERGENCE_REACHED = 5U
};

[[nodiscard]] const char* toString(StageSignal signal) noexcept;

struct ConvergenceSample {
  std::size_t iteration{0U};
  double verificationScore{0.0};
  double goalDistanceMetric{0.0};
  std::uint64_t planSignatureHash{0U};
  double deltaImprovement{0.0};
};

struct ConvergenceDecision {
  bool reached{false};
  StageSignal signal{StageSignal::Continue};
  bool verificationStagnated{false};
  bool goalDistanceStable{false};
  bool repeatedPlan{false};
  bool diminishingReturns{false};
  std::string reason;
};

class ConvergenceTracker {
 public:
  explicit ConvergenceTracker(std::size_t capacity = 4U);

  void observe(ConvergenceSample sample);

  [[nodiscard]] ConvergenceDecision evaluate(
      std::size_t maxStagnationIterations,
      double minImprovementDelta,
      std::size_t maxPlanRepetitions) const;

  [[nodiscard]] const std::vector<ConvergenceSample>& history() const noexcept;

 private:
  void trimHistory();

  std::size_t capacity_{0U};
  std::vector<ConvergenceSample> history_;
};

struct RepairRecord {
  std::size_t iteration{0U};
  std::size_t attempt{0U};
  std::string site;
  std::vector<std::string> taskIds;
  std::string decision;
  std::string reason;
  std::string overlayId;
  std::vector<std::string> affectedNodes;
  bool verified{false};
  double successRate{0.0};
};

struct ArbitrationDecisionRecord {
  std::size_t iteration{0U};
  std::size_t candidateCount{0U};
  std::size_t selectedCount{0U};
  std::size_t conflictCount{0U};
  std::vector<std::string> selectedTaskIds;
  std::string reason;
};

struct IntentConsistencyRecord {
  std::size_t iteration{0U};
  bool consistent{true};
  std::string planId;
  std::string reason;
};

struct FailureTrace {
  std::size_t iteration{0U};
  std::string phase;
  std::string module;
  std::string errorType;
  std::string rootCause;
  std::string taskId;
};

struct UltraLoopFrame {
  std::size_t iteration{0U};
  std::size_t retryCount{0U};
  bool failureDetected{false};
  bool verificationPassed{false};
  bool replanRequested{false};
  bool reanchorRequested{false};
  const ::ultra::runtime::CognitiveState* cognitiveState{nullptr};

  std::string intentGoal;
  std::string intentTarget;
  std::string intentBranchId;
  std::size_t intentTokenBudget{4096U};
  std::size_t intentImpactDepth{2U};
  std::size_t intentMaxFilesChanged{8U};
  ::ultra::runtime::intent::RiskTolerance intentTolerance{
      ::ultra::runtime::intent::RiskTolerance::MEDIUM};
  bool intentAllowPublicApiChange{false};
  double intentRiskThreshold{0.66};

  bool hasStructuredIntent{false};
  ::ultra::runtime::intent::Intent structuredIntent{};
  bool hasOriginalStructuredIntent{false};
  ::ultra::runtime::intent::Intent originalStructuredIntent{};
  nlohmann::ordered_json externalContextPayload =
      nlohmann::ordered_json::object();

  std::vector<TaskPayload> microTaskPayloads;
  bool hasTaskGraph{false};
  TaskGraph taskGraph{};
  bool hasExecutionResult{false};
  ::ultra::runtime::Result executionResult{};
  double verificationScore{0.0};
  double goalDistanceMetric{0.0};
  std::uint64_t planSignatureHash{0U};
  double deltaImprovement{0.0};
  ::ultra::runtime::intent::StrategyScore currentStrategyScore{};
  ::ultra::runtime::intent::StrategyFeedbackMemory latestStrategyFeedback{};
  std::vector<::ultra::runtime::intent::StrategyFeedbackMemory>
      strategyFeedbackLog;
  std::vector<::ultra::runtime::intent::PlanPerformance> planPerformanceHistory;
  std::size_t feedbackCommittedIteration{0U};

  std::string intentId;
  std::string originalIntentId;
  std::string strategyId;
  std::string planId;
  std::string executionId;
  std::string repairSite;
  std::vector<std::string> repairTaskIds;
  std::string repairDecision;
  std::string repairReason;
  bool intentConsistent{true};
  std::string intentConsistencyReason;
  std::vector<RepairRecord> repairLog;
  std::vector<ArbitrationDecisionRecord> arbitrationLog;
  std::vector<IntentConsistencyRecord> intentConsistencyLog;
};

struct StageResult {
  bool success{true};
  StageSignal signal{StageSignal::Continue};
  std::string message;
};

class IIntentStage {
 public:
  virtual ~IIntentStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IStrategyStage {
 public:
  virtual ~IStrategyStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IArbitrationStage {
 public:
  virtual ~IArbitrationStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IMicroPlanningStage {
 public:
  virtual ~IMicroPlanningStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IGovernanceStage {
 public:
  virtual ~IGovernanceStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IRecoveryStage {
 public:
  virtual ~IRecoveryStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IVerificationStage {
 public:
  virtual ~IVerificationStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IReflectionStage {
 public:
  virtual ~IReflectionStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IReanchorStage {
 public:
  virtual ~IReanchorStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IReplanningStage {
 public:
  virtual ~IReplanningStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

class IMemoryStage {
 public:
  virtual ~IMemoryStage() = default;
  virtual StageResult run(UltraLoopFrame& frame) = 0;
};

[[nodiscard]] IntentConsistencyRecord evaluateIntentConsistency(
    const UltraLoopFrame& frame);

class DeterministicArbitrationStage final : public IArbitrationStage {
 public:
  StageResult run(UltraLoopFrame& frame) override;
};

class DeterministicReanchorStage final : public IReanchorStage {
 public:
  StageResult run(UltraLoopFrame& frame) override;
};

struct UltraLoopBindings {
  IIntentStage* intent{nullptr};
  IStrategyStage* strategy{nullptr};
  IArbitrationStage* arbitration{nullptr};
  IMicroPlanningStage* microPlanning{nullptr};
  MicroPlanner* microPlanner{nullptr};
  ::ultra::runtime::ExecutionKernel* executionKernel{nullptr};
  IGovernanceStage* governance{nullptr};
  IRecoveryStage* recovery{nullptr};
  IVerificationStage* verification{nullptr};
  IReflectionStage* reflection{nullptr};
  IReanchorStage* reanchor{nullptr};
  IReplanningStage* replanning{nullptr};
  IMemoryStage* memory{nullptr};
  const ::ultra::memory::CognitiveMemoryManager* cognitiveMemory{nullptr};
};

struct TransitionRecord {
  UltraLoopState from{UltraLoopState::INIT};
  UltraLoopState to{UltraLoopState::INIT};
  std::string reason;
};

struct UltraLoopConfig {
  std::size_t maxIterations{8U};
  std::size_t maxRetriesPerIteration{2U};
  std::size_t maxStagnationIterations{2U};
  double minImprovementDelta{0.05};
  std::size_t maxPlanRepetitions{2U};
  std::function<void(UltraLoopState,
                     const StageResult&,
                     const UltraLoopFrame&)>
      failureHook;
  std::function<void(UltraLoopState, const std::string&)> statusHook;
};

struct UltraLoopReport {
  bool success{false};
  bool terminatedByIterationCap{false};
  UltraLoopState terminalState{UltraLoopState::TERMINATE};
  StageSignal terminalSignal{StageSignal::Continue};
  std::size_t iterations{0U};
  std::size_t retries{0U};
  std::vector<std::string> missingLayers;
  std::vector<TransitionRecord> transitions;
  std::vector<RepairRecord> repairs;
  std::vector<ArbitrationDecisionRecord> arbitration;
  std::vector<IntentConsistencyRecord> intentConsistency;
  std::vector<::ultra::runtime::intent::StrategyFeedbackMemory>
      strategyFeedback;
  std::vector<::ultra::runtime::intent::PlanPerformance> planPerformance;
  std::string message;
};

class UltraLoop {
 public:
  explicit UltraLoop(UltraLoopConfig config = {});

  [[nodiscard]] UltraLoopReport run(const UltraLoopBindings& bindings) const;

 private:
  [[nodiscard]] std::vector<std::string> detectMissingLayers(
      const UltraLoopBindings& bindings) const;

  UltraLoopConfig config_;
};

}  // namespace ultra::runtime::cognitive

