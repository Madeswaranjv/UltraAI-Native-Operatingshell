#include "CognitiveRuntime.h"

#include "../../core/state_manager.h"
#include "ExecutionKernel.h"
#include <external/json.hpp>
#include "failure_recovery.h"
#include "micro_planner.h"
#include "strategy_planner.h"
#include "ultra_loop.h"
#include "../governance/GovernanceEngine.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

[[nodiscard]] double riskValueForTolerance(
    const ::ultra::runtime::intent::RiskTolerance tolerance) noexcept {
  switch (tolerance) {
    case ::ultra::runtime::intent::RiskTolerance::LOW:
      return 0.25;
    case ::ultra::runtime::intent::RiskTolerance::MEDIUM:
      return 0.50;
    case ::ultra::runtime::intent::RiskTolerance::HIGH:
      return 0.75;
  }
  return 0.50;
}

[[nodiscard]] double riskThresholdForTolerance(
    const ::ultra::runtime::intent::RiskTolerance tolerance) noexcept {
  switch (tolerance) {
    case ::ultra::runtime::intent::RiskTolerance::LOW:
      return 0.33;
    case ::ultra::runtime::intent::RiskTolerance::MEDIUM:
      return 0.66;
    case ::ultra::runtime::intent::RiskTolerance::HIGH:
      return 0.85;
  }
  return 0.66;
}

[[nodiscard]] std::string confidenceFromReport(
    const ::ultra::runtime::cognitive::UltraLoopReport& report) {
  if (report.success && report.retries == 0U && !report.terminatedByIterationCap) {
    return "high";
  }
  if (report.success) {
    return "medium";
  }
  return "low";
}

[[nodiscard]] std::string joinViolations(
    const std::vector<std::string>& violations) {
  if (violations.empty()) {
    return {};
  }

  std::ostringstream stream;
  for (std::size_t index = 0U; index < violations.size(); ++index) {
    if (index > 0U) {
      stream << "; ";
    }
    stream << violations[index];
  }
  return stream.str();
}

[[nodiscard]] ::ultra::runtime::intent::ActionKind actionKindFromActionType(
    const ::ultra::runtime::ActionType type) noexcept {
  switch (type) {
    case ::ultra::runtime::ActionType::ImpactPrediction:
      return ::ultra::runtime::intent::ActionKind::ReduceImpactRadius;
    case ::ultra::runtime::ActionType::ContextExtraction:
      return ::ultra::runtime::intent::ActionKind::MinimizeTokenUsage;
    case ::ultra::runtime::ActionType::BranchDiff:
      return ::ultra::runtime::intent::ActionKind::ImproveCentrality;
    case ::ultra::runtime::ActionType::SimulateChange:
      return ::ultra::runtime::intent::ActionKind::RefactorModule;
    case ::ultra::runtime::ActionType::ModelGenerate:
      return ::ultra::runtime::intent::ActionKind::RefactorModule;
    case ::ultra::runtime::ActionType::ToolExecution:
      return ::ultra::runtime::intent::ActionKind::ModifySymbolBody;
    case ::ultra::runtime::ActionType::Mutation:
      return ::ultra::runtime::intent::ActionKind::ModifySymbolBody;
    case ::ultra::runtime::ActionType::IntentEvaluation:
      return ::ultra::runtime::intent::ActionKind::ModifySymbolBody;
  }
  return ::ultra::runtime::intent::ActionKind::ModifySymbolBody;
}

[[nodiscard]] ::ultra::runtime::intent::Strategy buildStrategyFromFrame(
    const ::ultra::runtime::cognitive::UltraLoopFrame& frame) {
  ::ultra::runtime::intent::Strategy strategy;
  strategy.name = frame.strategyId.empty() ? "runtime_strategy" : frame.strategyId;
  strategy.risk.classification = frame.intentTolerance;
  strategy.risk.tolerance = frame.intentTolerance;
  strategy.risk.value = riskValueForTolerance(frame.intentTolerance);

  for (const ::ultra::runtime::cognitive::TaskPayload& payload :
       frame.microTaskPayloads) {
    if (payload.kind != ::ultra::runtime::cognitive::TaskPayloadKind::Action) {
      continue;
    }

    ::ultra::runtime::intent::Action action;
    action.kind = actionKindFromActionType(payload.action.type);
    action.target = payload.action.target;
    if (action.target.empty()) {
      action.target = frame.intentTarget;
    }
    if (action.target.empty()) {
      action.target = frame.intentGoal;
    }
    if (action.target.empty()) {
      action.target = "workspace_root";
    }
    action.details = "Generated from deterministic runtime micro-task payload.";
    action.estimatedFilesChanged = 1U;
    action.estimatedDependencyDepth = 1U;
    action.publicApiSurface = frame.intentAllowPublicApiChange;
    strategy.proposedActions.push_back(std::move(action));
  }

  if (strategy.proposedActions.empty()) {
    ::ultra::runtime::intent::Action fallback;
    fallback.kind = ::ultra::runtime::intent::ActionKind::ModifySymbolBody;
    fallback.target = frame.intentTarget.empty() ? frame.intentGoal : frame.intentTarget;
    if (fallback.target.empty()) {
      fallback.target = "workspace_root";
    }
    fallback.details = "Fallback deterministic action from intent frame.";
    fallback.estimatedFilesChanged = 1U;
    fallback.estimatedDependencyDepth = 1U;
    fallback.publicApiSurface = frame.intentAllowPublicApiChange;
    strategy.proposedActions.push_back(std::move(fallback));
  }

  strategy.impact.radius = strategy.risk.value;
  strategy.impact.estimatedFiles = strategy.proposedActions.size();
  strategy.impact.dependencyDepth = 1U;
  strategy.impact.centrality = 0.0;
  strategy.impact.maxFilesConstraint = std::max<std::size_t>(
      1U, frame.intentMaxFilesChanged);
  strategy.impact.maxDepthConstraint = std::max<std::size_t>(
      1U, frame.intentImpactDepth);

  strategy.determinism.required = true;
  strategy.determinism.value = 1.0;

  strategy.tokenCost.budget = std::max<std::size_t>(1U, frame.intentTokenBudget);
  strategy.tokenCost.estimatedTokens = std::max<std::size_t>(
      1U, strategy.tokenCost.budget / 8U);
  strategy.tokenCost.withinBudget =
      strategy.tokenCost.estimatedTokens <= strategy.tokenCost.budget;

  return strategy;
}

class IntentSeedStage final : public ::ultra::runtime::cognitive::IIntentStage {
 public:
  IntentSeedStage(const ::ultra::runtime::intent::Intent& intentValue,
                  const ::ultra::runtime::CognitiveState& state,
                  std::string prompt)
      : intent_(intentValue), state_(state), prompt_(std::move(prompt)) {}

  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    frame.cognitiveState = &state_;
    frame.intentGoal = prompt_.empty() ? intent_.goal.target : prompt_;
    frame.intentTarget = intent_.goal.target.empty() ? frame.intentGoal
                                                      : intent_.goal.target;
    frame.intentBranchId = intent_.constraints.branchScope;
    frame.intentTokenBudget =
        std::max<std::size_t>(1U, intent_.constraints.tokenBudget);
    frame.intentImpactDepth =
        std::max<std::size_t>(1U, intent_.constraints.maxImpactDepth);
    frame.intentMaxFilesChanged =
        std::max<std::size_t>(1U, intent_.constraints.maxFilesChanged);
    frame.intentTolerance = intent_.risk;
    frame.intentAllowPublicApiChange = intent_.options.allowPublicAPIChange;
    frame.intentRiskThreshold = riskThresholdForTolerance(intent_.risk);

    frame.intentId = prompt_;
    if (frame.intentId.empty()) {
      frame.intentId = frame.intentGoal;
    }
    if (frame.intentId.empty()) {
      frame.intentId = frame.intentTarget;
    }
    if (frame.intentId.empty()) {
      frame.intentId = "intent:seed";
    }

    return {
        true,
        ::ultra::runtime::cognitive::StageSignal::Continue,
        "Intent stage seeded UltraLoop frame from CognitiveRuntime input.",
    };
  }

 private:
  const ::ultra::runtime::intent::Intent& intent_;
  const ::ultra::runtime::CognitiveState& state_;
  std::string prompt_;
};

class MicroPlanningGateStage final
    : public ::ultra::runtime::cognitive::IMicroPlanningStage {
 public:
  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    if (!frame.hasStructuredIntent) {
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Micro planning gate requires structured intent.",
      };
    }

    if (frame.microTaskPayloads.empty()) {
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Micro planning gate received no strategy payloads.",
      };
    }

    return {
        true,
        ::ultra::runtime::cognitive::StageSignal::Continue,
        "Micro planning gate accepted strategy payloads.",
    };
  }
};

class GovernanceStageAdapter final
    : public ::ultra::runtime::cognitive::IGovernanceStage {
 public:
  explicit GovernanceStageAdapter(
      ::ultra::runtime::governance::GovernanceEngine& engine) noexcept
      : engine_(engine) {}

  void setPolicy(const ::ultra::runtime::governance::Policy& policy) {
    policy_ = policy;
  }

  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    if (frame.cognitiveState == nullptr) {
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Governance stage requires a pinned cognitive state.",
      };
    }

    if (!frame.hasStructuredIntent) {
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Governance stage requires structured intent.",
      };
    }

    const ::ultra::runtime::intent::Strategy strategy = buildStrategyFromFrame(frame);
    const ::ultra::runtime::governance::GovernanceReport report =
        engine_.evaluate(strategy, policy_, *frame.cognitiveState);
    if (report.approved) {
      return {
          true,
          ::ultra::runtime::cognitive::StageSignal::Continue,
          report.reason.empty() ? "Governance approved execution strategy."
                                : report.reason,
      };
    }

    std::string message = report.reason.empty()
                              ? "Governance rejected execution strategy."
                              : report.reason;
    const std::string violations = joinViolations(report.violations);
    if (!violations.empty()) {
      message += " Violations: ";
      message += violations;
    }

    return {
        false,
        ::ultra::runtime::cognitive::StageSignal::Replan,
        std::move(message),
    };
  }

 private:
  ::ultra::runtime::governance::GovernanceEngine& engine_;
  ::ultra::runtime::governance::Policy policy_{};
};

class RecoveryStageAdapter final
    : public ::ultra::runtime::cognitive::IRecoveryStage {
 public:
  explicit RecoveryStageAdapter(const std::size_t retryLimit) noexcept
      : retryLimit_(retryLimit) {}

  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    ::ultra::runtime::cognitive::FailureContext context;
    context.task_id = frame.executionId.empty() ? "unknown_task" : frame.executionId;
    if (frame.hasExecutionResult) {
      context.execution_result = frame.executionResult;
    } else {
      context.execution_result.ok = false;
      context.execution_result.rolledBack = false;
      context.execution_result.message =
          "Recovery stage invoked without execution result.";
    }

    context.retry_count = frame.retryCount;
    context.retry_limit = retryLimit_;
    context.dependency_state = ::ultra::runtime::cognitive::DependencyState{
        frame.taskGraph.has_pending_tasks(),
        frame.taskGraph.failed_tasks().size(),
    };

    const ::ultra::runtime::cognitive::RecoveryAction action =
        recovery_.decide(context);
    switch (action) {
      case ::ultra::runtime::cognitive::RecoveryAction::RETRY_TASK:
        return {
            true,
            ::ultra::runtime::cognitive::StageSignal::Retry,
            "Recovery requested retry for deterministic task execution.",
        };
      case ::ultra::runtime::cognitive::RecoveryAction::SKIP_TASK:
        return {
            true,
            ::ultra::runtime::cognitive::StageSignal::Replan,
            "Recovery requested SKIP_TASK; forwarding to replanning.",
        };
      case ::ultra::runtime::cognitive::RecoveryAction::REPLAN_REQUIRED:
        return {
            true,
            ::ultra::runtime::cognitive::StageSignal::Replan,
            "Recovery requested deterministic replanning.",
        };
      case ::ultra::runtime::cognitive::RecoveryAction::ABORT_LOOP:
        return {
            false,
            ::ultra::runtime::cognitive::StageSignal::TerminateFailure,
            "Recovery requested loop termination.",
        };
    }

    return {
        false,
        ::ultra::runtime::cognitive::StageSignal::TerminateFailure,
        "Recovery stage produced an unknown action.",
    };
  }

 private:
  std::size_t retryLimit_{2U};
  ::ultra::runtime::cognitive::FailureRecoveryEngine recovery_{};
};

class VerificationStageAdapter final
    : public ::ultra::runtime::cognitive::IVerificationStage {
 public:
  explicit VerificationStageAdapter(const std::size_t retryLimit) noexcept
      : retryLimit_(retryLimit) {}

  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    if (!frame.hasTaskGraph) {
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Verification stage requires a task graph.",
      };
    }

    if (!frame.taskGraph.has_pending_tasks() && frame.hasExecutionResult &&
        frame.executionResult.ok && !frame.executionResult.rolledBack) {
      return {
          true,
          ::ultra::runtime::cognitive::StageSignal::Continue,
          "Verification accepted deterministic execution result.",
      };
    }

    if (!frame.taskGraph.failed_tasks().empty() && frame.retryCount < retryLimit_) {
      return {
          true,
          ::ultra::runtime::cognitive::StageSignal::Retry,
          "Verification requested retry for failed task graph nodes.",
      };
    }

    std::string message = frame.hasExecutionResult
                              ? frame.executionResult.message
                              : "Verification stage has no execution result.";
    if (message.empty()) {
      message = "Verification requested deterministic replanning.";
    }

    return {
        false,
        ::ultra::runtime::cognitive::StageSignal::Replan,
        std::move(message),
    };
  }

 private:
  std::size_t retryLimit_{2U};
};

class ReflectionStageAdapter final
    : public ::ultra::runtime::cognitive::IReflectionStage {
 public:
  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    if (!frame.verificationPassed) {
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Reflection requires a passing verification stage.",
      };
    }

    std::string message = frame.hasExecutionResult ? frame.executionResult.message
                                                   : std::string{};
    if (message.empty()) {
      message = "Reflection completed deterministic loop summary.";
    }

    return {
        true,
        ::ultra::runtime::cognitive::StageSignal::Continue,
        std::move(message),
    };
  }
};

class ReplanningStageAdapter final
    : public ::ultra::runtime::cognitive::IReplanningStage {
 public:
  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    if (!frame.replanRequested) {
      return {
          true,
          ::ultra::runtime::cognitive::StageSignal::Continue,
          "Replanning stage bypassed; no replan requested.",
      };
    }

    return {
        true,
        ::ultra::runtime::cognitive::StageSignal::Continue,
        "Replanning stage accepted deterministic retry path.",
    };
  }
};

class MemoryStageAdapter final : public ::ultra::runtime::cognitive::IMemoryStage {
 public:
  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    if (frame.cognitiveState != nullptr && frame.cognitiveState->workingSet) {
      frame.cognitiveState->workingSet->syncVersions(
          frame.cognitiveState->pinnedVersion);
    }

    return {
        true,
        ::ultra::runtime::cognitive::StageSignal::Continue,
        "Memory checkpoint synchronized with pinned runtime state.",
    };
  }
};

}  // namespace

namespace ultra::runtime {

SnapshotPinGuard::SnapshotPinGuard(const core::StateManager& stateManager,
                                   const CognitiveState& state)
    : stateManager_(stateManager), state_(state) {
  assertCurrent();
}

void SnapshotPinGuard::assertCurrent() const {
  if (state_.snapshot.version != state_.pinnedVersion) {
    throw std::runtime_error(
        "Snapshot pin mismatch: pinned version diverges from state snapshot.");
  }
  stateManager_.ensureSnapshotCurrent(state_.snapshot);
  const std::string livePinnedHash = state_.snapshot.deterministicHash();
  if (livePinnedHash != state_.pinnedHash) {
    throw std::runtime_error(
        "Snapshot pin mismatch: deterministic hash diverges from pinned hash.");
  }
}

CognitiveRuntime::CognitiveRuntime(core::StateManager& stateManager) noexcept
    : stateManager_(stateManager) {}

CognitiveState CognitiveRuntime::createState(
    const TokenBudget budget,
    const RelevanceProfile& weights) const {
  return stateManager_.createCognitiveState(
      static_cast<std::size_t>(budget), weights);
}

SnapshotPinGuard CognitiveRuntime::pin(const CognitiveState& state) const {
  return SnapshotPinGuard(stateManager_, state);
}

const cognitive::tools::ToolRegistry& CognitiveRuntime::toolRegistry() const noexcept {
  return toolRegistry_;
}

GovernanceSignal CognitiveRuntime::currentGovernanceSignal() const {
  CPUGovernor& governor = CPUGovernor::instance();
  const GovernorStats stats = governor.stats();

  GovernanceSignal signal;
  signal.idle = stats.idle;
  signal.activeWorkloads = stats.activeWorkloads;
  signal.hardwareThreads = std::max<std::size_t>(1U, stats.hardwareThreads);
  signal.recommendedThreads = governor.recommendedThreadCount(
      signal.hardwareThreads, "governance.tool_execution");
  signal.movingAverageMs = stats.movingAverageMs;
  return signal;
}

CognitiveLoopResult CognitiveRuntime::run(
    const intent::Intent& intentValue,
    const governance::Policy& policy) {
  CognitiveLoopResult result;

  try {
    const std::size_t fallbackBudget =
        intentValue.constraints.tokenBudget == 0U
            ? 4096U
            : intentValue.constraints.tokenBudget;
    const intent::Intent normalizedIntent =
        intent::normalizeIntent(intentValue, fallbackBudget);

    const CognitiveState state = createState(normalizedIntent.constraints.tokenBudget);
    const SnapshotPinGuard guard = pin(state);
    guard.assertCurrent();

    ExecutionKernel executionKernel(stateManager_);
    cognitive::MicroPlanner microPlanner;
    cognitive::StrategyPlanningStage strategyStage;
    governance::GovernanceEngine governanceEngine(&stateManager_.cognitiveMemory());

    const std::string prompt = normalizedIntent.goal.target.empty()
                                   ? intent::toString(normalizedIntent.goal.type)
                                   : normalizedIntent.goal.target;

    IntentSeedStage intentStage(normalizedIntent, state, prompt);
    MicroPlanningGateStage microPlanningStage;
    GovernanceStageAdapter governanceStage(governanceEngine);
    governanceStage.setPolicy(policy);
    RecoveryStageAdapter recoveryStage(2U);
    VerificationStageAdapter verificationStage(2U);
    ReflectionStageAdapter reflectionStage;
    ReplanningStageAdapter replanningStage;
    MemoryStageAdapter memoryStage;

    cognitive::UltraLoopBindings bindings;
    bindings.intent = &intentStage;
    bindings.strategy = &strategyStage;
    bindings.microPlanning = &microPlanningStage;
    bindings.microPlanner = &microPlanner;
    bindings.executionKernel = &executionKernel;
    bindings.governance = &governanceStage;
    bindings.recovery = &recoveryStage;
    bindings.verification = &verificationStage;
    bindings.reflection = &reflectionStage;
    bindings.replanning = &replanningStage;
    bindings.memory = &memoryStage;

    cognitive::UltraLoopConfig config;
    config.maxIterations = std::max<std::size_t>(
        1U, static_cast<std::size_t>(std::max(1, policy.maxImpactDepth)));
    config.maxRetriesPerIteration = 2U;

    cognitive::UltraLoop loop(config);
    const cognitive::UltraLoopReport report = loop.run(bindings);

    result.ok = report.success;
    result.verifyStatus = report.success ? "PASS" : "FAIL";
    result.confidence = confidenceFromReport(report);

    // Extract model text output from the last successful execution result.
    // ExecutionKernel caches its last ok result — we read it here so we
    // never make a second LLM call.
    std::string modelTextOutput;
    if (executionKernel.hasLastResult()) {
      const nlohmann::ordered_json& payload = executionKernel.lastResult().payload;
      // Walk child_results looking for a ModelGenerate entry
      if (payload.contains("child_results") &&
          payload.at("child_results").is_array()) {
        for (const auto& child : payload.at("child_results")) {
          if (!child.is_object()) { continue; }
          const bool isModelGenerate =
              child.contains("type") &&
              child.at("type").is_string() &&
              child.at("type").get<std::string>() == "ModelGenerate";
          if (!isModelGenerate) { continue; }
          if (!child.contains("payload") ||
              !child.at("payload").is_object()) { continue; }
          const auto& childPayload = child.at("payload");
          if (!childPayload.contains("response") ||
              !childPayload.at("response").is_object()) { continue; }
          const auto& resp = childPayload.at("response");
          if (resp.contains("text_output") &&
              resp.at("text_output").is_string()) {
            modelTextOutput = resp.at("text_output").get<std::string>();
            if (!modelTextOutput.empty()) { break; }
          }
        }
      }
      // Also handle direct ModelGenerate (not wrapped in IntentEvaluation)
      if (modelTextOutput.empty() &&
          payload.contains("response") &&
          payload.at("response").is_object()) {
        const auto& resp = payload.at("response");
        if (resp.contains("text_output") && resp.at("text_output").is_string()) {
          modelTextOutput = resp.at("text_output").get<std::string>();
        }
      }
    }

    result.output = modelTextOutput.empty() ? report.message : modelTextOutput;

    if (!report.missingLayers.empty()) {
      if (!result.output.empty()) { result.output += "\n"; }
      result.output += "Missing layers: ";
      result.output += joinViolations(report.missingLayers);
    }
    if (!report.success) {
      result.errorMessage = report.message.empty()
                                ? "UltraLoop execution failed."
                                : report.message;
    }
  } catch (const std::exception& ex) {
    result.ok = false;
    result.verifyStatus = "FAIL";
    result.confidence = "low";
    result.errorMessage = ex.what();
  } catch (...) {
    result.ok = false;
    result.verifyStatus = "FAIL";
    result.confidence = "low";
    result.errorMessage = "Cognitive runtime failed with an unknown error.";
  }

  return result;
}

}  // namespace ultra::runtime