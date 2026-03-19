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

namespace ultra::runtime::cognitive {

enum class UltraLoopState : std::uint8_t {
  INIT = 0U,
  PLAN = 1U,
  MICRO_PLAN = 2U,
  EXECUTE = 3U,
  VERIFY = 4U,
  REFLECT = 5U,
  REPLAN = 6U,
  TERMINATE = 7U
};

[[nodiscard]] const char* toString(UltraLoopState state) noexcept;

enum class StageSignal : std::uint8_t {
  Continue = 0U,
  Retry = 1U,
  Replan = 2U,
  TerminateSuccess = 3U,
  TerminateFailure = 4U
};

[[nodiscard]] const char* toString(StageSignal signal) noexcept;

struct UltraLoopFrame {
  std::size_t iteration{0U};
  std::size_t retryCount{0U};
  bool failureDetected{false};
  bool verificationPassed{false};
  bool replanRequested{false};
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

  std::vector<TaskPayload> microTaskPayloads;
  bool hasTaskGraph{false};
  TaskGraph taskGraph{};
  bool hasExecutionResult{false};
  ::ultra::runtime::Result executionResult{};

  std::string intentId;
  std::string strategyId;
  std::string planId;
  std::string executionId;
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

struct UltraLoopBindings {
  IIntentStage* intent{nullptr};
  IStrategyStage* strategy{nullptr};
  IMicroPlanningStage* microPlanning{nullptr};
  MicroPlanner* microPlanner{nullptr};
  ::ultra::runtime::ExecutionKernel* executionKernel{nullptr};
  IGovernanceStage* governance{nullptr};
  IRecoveryStage* recovery{nullptr};
  IVerificationStage* verification{nullptr};
  IReflectionStage* reflection{nullptr};
  IReplanningStage* replanning{nullptr};
  IMemoryStage* memory{nullptr};
};

struct TransitionRecord {
  UltraLoopState from{UltraLoopState::INIT};
  UltraLoopState to{UltraLoopState::INIT};
  std::string reason;
};

struct UltraLoopConfig {
  std::size_t maxIterations{8U};
  std::size_t maxRetriesPerIteration{2U};
  std::function<void(UltraLoopState,
                     const StageResult&,
                     const UltraLoopFrame&)>
      failureHook;
};

struct UltraLoopReport {
  bool success{false};
  bool terminatedByIterationCap{false};
  UltraLoopState terminalState{UltraLoopState::TERMINATE};
  std::size_t iterations{0U};
  std::size_t retries{0U};
  std::vector<std::string> missingLayers;
  std::vector<TransitionRecord> transitions;
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
