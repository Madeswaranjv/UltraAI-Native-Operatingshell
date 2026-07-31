#include "CognitiveRuntime.h"

#include "../../core/state_manager.h"
#include "../../adapters/AdapterFactory.h"
#include "../../adapters/BuildErrorParser.h"
#include "../../core/ProjectTypeDetector.h"
#include "../../cli/CommandOptions.h"
#include "ExecutionKernel.h"
#include <external/json.hpp>
#include "failure_recovery.h"
#include "micro_planner.h"
#include "strategy_planner.h"
#include "ultra_loop.h"
#include "../governance/GovernanceEngine.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
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

[[nodiscard]] std::string formatUtcTimestamp(
    const std::chrono::system_clock::time_point timestamp) {
  const std::time_t rawTime = std::chrono::system_clock::to_time_t(timestamp);
  std::tm utcTime{};
#if defined(_WIN32)
  gmtime_s(&utcTime, &rawTime);
#else
  gmtime_r(&rawTime, &utcTime);
#endif
  std::ostringstream stream;
  stream << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

[[nodiscard]] std::string collapseFailureText(std::string value,
                                              const bool underscoreSpaces) {
  std::string collapsed;
  collapsed.reserve(value.size());
  bool previousWhitespace = false;
  for (const char ch : value) {
    if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
      if (underscoreSpaces) {
        if (!collapsed.empty() && collapsed.back() != '_') {
          collapsed.push_back('_');
        }
      } else if (!previousWhitespace) {
        collapsed.push_back(' ');
      }
      previousWhitespace = true;
      continue;
    }
    collapsed.push_back(ch);
    previousWhitespace = false;
  }

  const char trimChar = underscoreSpaces ? '_' : ' ';
  const auto trimPos = collapsed.find_first_not_of(trimChar);
  if (trimPos == std::string::npos) {
    return "unknown";
  }
  const auto trimEnd = collapsed.find_last_not_of(trimChar);
  return collapsed.substr(trimPos, trimEnd - trimPos + 1U);
}

[[nodiscard]] std::string failureModuleForFrame(
    const ::ultra::runtime::cognitive::UltraLoopFrame& frame) {
  if (frame.hasExecutionResult && frame.executionResult.payload.is_object()) {
    if (frame.executionResult.payload.contains("tool") &&
        frame.executionResult.payload.at("tool").is_string()) {
      return frame.executionResult.payload.at("tool").get<std::string>();
    }
  }
  if (!frame.executionId.empty()) {
    return frame.executionId;
  }
  if (!frame.repairSite.empty()) {
    return frame.repairSite;
  }
  if (!frame.intentTarget.empty()) {
    return frame.intentTarget;
  }
  return "runtime";
}

[[nodiscard]] std::string failureRootCauseForFrame(
    const ::ultra::runtime::cognitive::StageResult& stageResult,
    const ::ultra::runtime::cognitive::UltraLoopFrame& frame) {
  if (frame.hasExecutionResult) {
    const auto& payload = frame.executionResult.payload;
    if (payload.is_object()) {
      if (payload.contains("output_json") && payload.at("output_json").is_object() &&
          payload.at("output_json").contains("error") &&
          payload.at("output_json").at("error").is_string()) {
        return payload.at("output_json").at("error").get<std::string>();
      }
      if (payload.contains("governance") && payload.at("governance").is_object() &&
          payload.at("governance").contains("reason") &&
          payload.at("governance").at("reason").is_string()) {
        return payload.at("governance").at("reason").get<std::string>();
      }
      if (payload.contains("response") && payload.at("response").is_object() &&
          payload.at("response").contains("error_message") &&
          payload.at("response").at("error_message").is_string()) {
        return payload.at("response").at("error_message").get<std::string>();
      }
      if (payload.contains("output") && payload.at("output").is_string()) {
        return payload.at("output").get<std::string>();
      }
    }
    if (!frame.executionResult.message.empty()) {
      return frame.executionResult.message;
    }
  }
  return stageResult.message;
}

[[nodiscard]] std::string failureErrorTypeForFrame(
    const ::ultra::runtime::cognitive::UltraLoopState phase,
    const ::ultra::runtime::cognitive::StageResult& stageResult,
    const ::ultra::runtime::cognitive::UltraLoopFrame& frame) {
  if (phase == ::ultra::runtime::cognitive::UltraLoopState::VERIFY) {
    return "verification_failed";
  }
  if (phase == ::ultra::runtime::cognitive::UltraLoopState::PARTIAL_REPAIR) {
    return "repair_failed";
  }
  if (phase == ::ultra::runtime::cognitive::UltraLoopState::EXECUTE &&
      frame.hasExecutionResult) {
    switch (frame.executionResult.type) {
      case ::ultra::runtime::ActionType::ToolExecution:
        return "tool_execution_failed";
      case ::ultra::runtime::ActionType::ModelGenerate:
        return "model_generation_failed";
      case ::ultra::runtime::ActionType::SimulateChange:
        return "simulation_path_blocked";
      default:
        return "execution_failed";
    }
  }
  if (stageResult.signal == ::ultra::runtime::cognitive::StageSignal::Replan) {
    return "replan_required";
  }
  if (stageResult.signal ==
      ::ultra::runtime::cognitive::StageSignal::TerminateFailure) {
    return "terminate_failure";
  }
  return "stage_failure";
}

[[nodiscard]] ::ultra::runtime::cognitive::FailureTrace buildFailureTrace(
    const ::ultra::runtime::cognitive::UltraLoopState phase,
    const ::ultra::runtime::cognitive::StageResult& stageResult,
    const ::ultra::runtime::cognitive::UltraLoopFrame& frame) {
  ::ultra::runtime::cognitive::FailureTrace trace;
  trace.iteration = frame.iteration;
  trace.phase = ::ultra::runtime::cognitive::toString(phase);
  trace.module = collapseFailureText(failureModuleForFrame(frame), true);
  trace.errorType = collapseFailureText(
      failureErrorTypeForFrame(phase, stageResult, frame),
      true);
  trace.rootCause = collapseFailureText(
      failureRootCauseForFrame(stageResult, frame),
      false);
  trace.taskId = frame.executionId;
  return trace;
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

  std::size_t totalEstimatedFiles = 0U;
  std::size_t maxEstimatedDepth = 1U;
  for (const ::ultra::runtime::cognitive::TaskPayload& payload :
       frame.microTaskPayloads) {
    if (payload.kind != ::ultra::runtime::cognitive::TaskPayloadKind::Action) {
      continue;
    }

    ::ultra::runtime::intent::Action action;
    if (payload.plannedAction.has_value()) {
      action = *payload.plannedAction;
    } else {
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
    }

    totalEstimatedFiles += std::max<std::size_t>(1U, action.estimatedFilesChanged);
    maxEstimatedDepth = std::max<std::size_t>(
        maxEstimatedDepth,
        std::max<std::size_t>(1U, action.estimatedDependencyDepth));
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
  strategy.impact.estimatedFiles = std::max<std::size_t>(
      strategy.proposedActions.empty() ? 1U : totalEstimatedFiles,
      strategy.proposedActions.size());
  strategy.impact.dependencyDepth = maxEstimatedDepth;
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
                  std::string prompt,
                  nlohmann::ordered_json externalContextPayload)
      : intent_(intentValue),
        state_(state),
        prompt_(std::move(prompt)),
        externalContextPayload_(std::move(externalContextPayload)) {}

  ::ultra::runtime::cognitive::StageResult run(
      ::ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    frame.cognitiveState = &state_;
    frame.structuredIntent = intent_;
    frame.hasStructuredIntent = true;
    frame.intentGoal = intent_.rawPrompt.empty() ? (intent_.goal.target.empty() ? prompt_ : intent_.goal.target) : intent_.rawPrompt;
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

    frame.intentId = ::ultra::runtime::intent::toString(intent_.goal.type);
    if (!frame.intentTarget.empty()) {
      frame.intentId += ":" + frame.intentTarget;
    } else if (!frame.intentGoal.empty()) {
      frame.intentId += ":" + frame.intentGoal;
    }
    if (frame.intentId.empty()) {
      frame.intentId = "intent:seed";
    }

    frame.originalStructuredIntent = intent_;
    frame.hasOriginalStructuredIntent = true;
    frame.originalIntentId = frame.intentId;
    frame.externalContextPayload = externalContextPayload_;

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
  nlohmann::ordered_json externalContextPayload_ =
      nlohmann::ordered_json::object();
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

    if (frame.taskGraph.has_pending_tasks() || (!frame.taskGraph.failed_tasks().empty() && frame.retryCount < retryLimit_)) {
        return {
            true,
            ::ultra::runtime::cognitive::StageSignal::Retry,
            "Verification requested retry for failed task graph nodes.",
        };
    }

    // --- REAL VERIFICATION ---
    std::cout << "\n[ULTRA] VERIFY: Running workspace build and test verification...\n";

    std::error_code ec;
    const std::filesystem::path rootPath = std::filesystem::current_path(ec);
    
    const ::ultra::core::ProjectType projectType = 
        ::ultra::core::ProjectTypeDetector::detect(rootPath);
    
    std::unique_ptr<::ultra::adapters::ProjectAdapter> adapter = 
        ::ultra::adapters::createProjectAdapter(projectType, rootPath);

    ::ultra::cli::CommandOptions options;
    options.jsonOutput = true; // prevent printing duplicate logs if we capture
    
    // 1. Run Build
    adapter->build(options);
    const int buildExitCode = adapter->lastExitCode();
    const std::string buildOutput = adapter->lastOutput();

    if (buildExitCode != 0) {
      std::cout << "[ULTRA] VERIFY FAILED: Build error detected.\n";
      frame.hasExecutionResult = true;
      frame.executionResult.ok = false;
      frame.executionResult.message = "Build failed with exit code " + std::to_string(buildExitCode) + "\n" + buildOutput;
      frame.executionResult.text_output = buildOutput;
      
      if (frame.retryCount < retryLimit_) {
        return {
            false,
            ::ultra::runtime::cognitive::StageSignal::Retry,
            "Verification failed during build. Retrying...",
        };
      }
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Verification failed during build.",
      };
    }

    std::cout << "[ULTRA] VERIFY: Build passed. Running tests...\n";

    // 2. Run Tests
    adapter->test(options);
    const int testExitCode = adapter->lastExitCode();
    const std::string testOutput = adapter->lastOutput();

    if (testExitCode != 0) {
      std::cout << "[ULTRA] VERIFY FAILED: Test error detected.\n";
      frame.hasExecutionResult = true;
      frame.executionResult.ok = false;
      const std::string conciseError = ::ultra::adapters::BuildErrorParser::parse(testOutput);
      frame.executionResult.message = "Tests failed with exit code " + std::to_string(testExitCode) + "\n" + conciseError;
      frame.executionResult.text_output = conciseError + "\n\n--- RAW LOGS OMITTED FOR TOKEN EFFICIENCY ---";
      
      if (frame.retryCount < retryLimit_) {
        return {
            false,
            ::ultra::runtime::cognitive::StageSignal::Retry,
            "Verification failed during test. Retrying...",
        };
      }
      return {
          false,
          ::ultra::runtime::cognitive::StageSignal::Replan,
          "Verification failed during test.",
      };
    }

    std::cout << "[ULTRA] VERIFY PASSED: Build and tests successful.\n";
    frame.hasExecutionResult = true;
    frame.executionResult.ok = true;
    frame.executionResult.rolledBack = false;
    frame.executionResult.message = "Verification successful.";

    frame.verificationPassed = true;

    return {
        true,
        ::ultra::runtime::cognitive::StageSignal::Continue,
        "Verification accepted deterministic execution result.",
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

    const ::ultra::runtime::cognitive::IntentConsistencyRecord consistency =
        ::ultra::runtime::cognitive::evaluateIntentConsistency(frame);
    frame.intentConsistencyLog.push_back(consistency);
    frame.intentConsistent = consistency.consistent;
    frame.intentConsistencyReason = consistency.reason;
    frame.reanchorRequested = !consistency.consistent;

    std::string message = frame.hasExecutionResult ? frame.executionResult.message
                                                   : std::string{};
    if (message.empty()) {
      message = "Reflection completed deterministic loop summary.";
    }
    if (!consistency.consistent) {
      message = consistency.reason;
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
    const governance::Policy& policy,
    std::function<void(const std::string&)> statusHook,
    nlohmann::ordered_json externalContextPayload) {
  CognitiveLoopResult result;
  const auto executionStartWall = std::chrono::system_clock::now();
  const auto executionStartMonotonic = std::chrono::steady_clock::now();
  std::vector<cognitive::FailureTrace> failureTraces;

  const auto finalizeResult = [&result,
                               &failureTraces,
                               executionStartWall,
                               executionStartMonotonic]() {
    result.executionStartTime = formatUtcTimestamp(executionStartWall);
    result.executionEndTime =
        formatUtcTimestamp(std::chrono::system_clock::now());
    result.executionDurationSeconds = std::chrono::duration<double>(
                                        std::chrono::steady_clock::now() -
                                        executionStartMonotonic)
                                        .count();
    result.failureTraces = failureTraces;
  };

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

    const std::string prompt = normalizedIntent.rawPrompt.empty() ? (normalizedIntent.goal.target.empty() ? intent::toString(normalizedIntent.goal.type) : normalizedIntent.goal.target) : normalizedIntent.rawPrompt;



    IntentSeedStage intentStage(normalizedIntent,
                                state,
                                prompt,
                                std::move(externalContextPayload));
    MicroPlanningGateStage microPlanningStage;
    GovernanceStageAdapter governanceStage(governanceEngine);
    governanceStage.setPolicy(policy);
    RecoveryStageAdapter recoveryStage(2U);
    VerificationStageAdapter verificationStage(2U);
    ReflectionStageAdapter reflectionStage;
    cognitive::DeterministicArbitrationStage arbitrationStage;
    cognitive::DeterministicReanchorStage reanchorStage;
    ReplanningStageAdapter replanningStage;
    MemoryStageAdapter memoryStage;

    cognitive::UltraLoopBindings bindings;
    bindings.intent = &intentStage;
    bindings.strategy = &strategyStage;
    bindings.arbitration = &arbitrationStage;
    bindings.microPlanning = &microPlanningStage;
    bindings.microPlanner = &microPlanner;
    bindings.executionKernel = &executionKernel;
    bindings.governance = &governanceStage;
    bindings.recovery = &recoveryStage;
    bindings.verification = &verificationStage;
    bindings.reflection = &reflectionStage;
    bindings.reanchor = &reanchorStage;
    bindings.replanning = &replanningStage;
    bindings.memory = &memoryStage;
    bindings.cognitiveMemory = &stateManager_.cognitiveMemory();

    cognitive::UltraLoopConfig config;
    config.maxStagnationIterations = 2U;
    config.minImprovementDelta = 0.05;
    config.maxPlanRepetitions = 2U;
    config.maxIterations = std::max<std::size_t>(
        std::max<std::size_t>(config.maxStagnationIterations + 1U,
                              config.maxPlanRepetitions + 1U),
        static_cast<std::size_t>(std::max(2, policy.maxImpactDepth)));
    config.maxRetriesPerIteration = 2U;
    config.failureHook = [&failureTraces](
                             const cognitive::UltraLoopState phase,
                             const cognitive::StageResult& stageResult,
                             const cognitive::UltraLoopFrame& frame) {
      const cognitive::FailureTrace trace =
          buildFailureTrace(phase, stageResult, frame);
      failureTraces.push_back(trace);
      std::cerr << "[FAILURE TRACE] phase="
                << collapseFailureText(trace.phase, true)
                << " module=" << collapseFailureText(trace.module, true)
                << " error_type=" << collapseFailureText(trace.errorType, true)
                << " root_cause=" << collapseFailureText(trace.rootCause, false)
                << "\n";
    };
    config.statusHook = [statusHook](const cognitive::UltraLoopState /*state*/,
                                     const std::string& message) {
      if (statusHook) {
        statusHook(message);
      }
    };

    cognitive::UltraLoop loop(config);
    const cognitive::UltraLoopReport report = loop.run(bindings);

    result.ok = report.success;
    result.verifyStatus = report.success ? "PASS" : "FAIL";
    result.confidence = confidenceFromReport(report);
    result.transitions = report.transitions;
    result.repairs = report.repairs;
    result.arbitration = report.arbitration;
    result.intentConsistency = report.intentConsistency;

    std::string modelTextOutput = executionKernel.lastModelTextOutput();
    std::string providerUsed = executionKernel.lastSelectedProvider();
    std::string providerEndpoint = executionKernel.lastProviderEndpoint();
    const auto assignModelTextOutput = [&modelTextOutput](
                                       const nlohmann::ordered_json& responsePayload) {
      if (!responsePayload.is_object() || !modelTextOutput.empty()) {
        return;
      }
      if (responsePayload.contains("text_output") &&
          responsePayload.at("text_output").is_string()) {
        modelTextOutput = responsePayload.at("text_output").get<std::string>();
        return;
      }
      if (responsePayload.contains("textOutput") &&
          responsePayload.at("textOutput").is_string()) {
        modelTextOutput = responsePayload.at("textOutput").get<std::string>();
        return;
      }
      if (responsePayload.contains("output") &&
          responsePayload.at("output").is_string()) {
        modelTextOutput = responsePayload.at("output").get<std::string>();
      }
    };
    const auto assignProviderMetadata =
        [&providerUsed, &providerEndpoint](const nlohmann::ordered_json& payload) {
          if (!payload.is_object()) {
            return;
          }
          if (providerUsed.empty() &&
              payload.contains("selected_provider") &&
              payload.at("selected_provider").is_string()) {
            providerUsed = payload.at("selected_provider").get<std::string>();
          }
          if (providerEndpoint.empty() &&
              payload.contains("provider_endpoint") &&
              payload.at("provider_endpoint").is_string()) {
            providerEndpoint = payload.at("provider_endpoint").get<std::string>();
          }
          if (providerUsed.empty() &&
              payload.contains("provider_hint") &&
              payload.at("provider_hint").is_string()) {
            providerUsed = payload.at("provider_hint").get<std::string>();
          }
        };
    if (executionKernel.hasLastResult()) {
      const auto& lastExecutionResult = executionKernel.lastResult();
      const nlohmann::ordered_json& payload = lastExecutionResult.payload;
      result.executionPayload = payload;
      if (payload.contains("tool_call_detected") &&
          payload.at("tool_call_detected").is_boolean()) {
        result.toolCallDetected = payload.at("tool_call_detected").get<bool>();
      }
      if (payload.contains("tool_router_executed") &&
          payload.at("tool_router_executed").is_boolean()) {
        result.toolRouterExecuted = payload.at("tool_router_executed").get<bool>();
      }
      if (payload.contains("tool_execution") &&
          payload.at("tool_execution").is_object()) {
        result.toolExecution = payload.at("tool_execution");
      } else if (payload.contains("output_json") &&
                 payload.at("output_json").is_object()) {
        result.toolExecution = payload.at("output_json");
        if (!result.toolExecution.contains("tool") &&
            payload.contains("tool") &&
            payload.at("tool").is_string()) {
          result.toolExecution["tool"] = payload.at("tool").get<std::string>();
        }
      }

      const bool hasToolExecutionPayload =
          result.toolCallDetected || result.toolRouterExecuted ||
          (result.toolExecution.is_object() && !result.toolExecution.empty());

      if (!hasToolExecutionPayload && !lastExecutionResult.text_output.empty()) {
        modelTextOutput = lastExecutionResult.text_output;
      }

      assignProviderMetadata(payload);
      if (!hasToolExecutionPayload &&
          modelTextOutput.empty() &&
          payload.contains("text_output") &&
          payload.at("text_output").is_string()) {
        modelTextOutput = payload.at("text_output").get<std::string>();
      }

      if (!hasToolExecutionPayload &&
          payload.contains("child_results") &&
          payload.at("child_results").is_array()) {
        for (const auto& child : payload.at("child_results")) {
          if (!child.is_object()) {
            continue;
          }
          const bool isModelGenerate =
              child.contains("type") &&
              child.at("type").is_string() &&
              child.at("type").get<std::string>() == "ModelGenerate";
          if (!isModelGenerate) {
            continue;
          }
          if (!child.contains("payload") ||
              !child.at("payload").is_object()) {
            continue;
          }
          const auto& childPayload = child.at("payload");
          assignProviderMetadata(childPayload);
          if (!childPayload.contains("response") ||
              !childPayload.at("response").is_object()) {
            continue;
          }
          assignModelTextOutput(childPayload.at("response"));
          if (!modelTextOutput.empty()) {
            break;
          }
        }
      }
      if (!hasToolExecutionPayload &&
          modelTextOutput.empty() &&
          payload.contains("response") &&
          payload.at("response").is_object()) {
        assignModelTextOutput(payload.at("response"));
      }
    }


    result.llm_output = modelTextOutput;
    result.providerUsed = providerUsed;
    result.providerEndpoint = providerEndpoint;
    result.executionSummary = report.message;

    if (!report.missingLayers.empty()) {
      if (!result.executionSummary.empty()) {
        result.executionSummary += "\n";
      }
      result.executionSummary += "Missing layers: ";
      result.executionSummary += joinViolations(report.missingLayers);
    }
    const bool hasToolExecution =
        result.toolCallDetected || result.toolRouterExecuted ||
        (result.toolExecution.is_object() && !result.toolExecution.empty());
    const bool preferLlmOutput =
        report.success && !hasToolExecution && !modelTextOutput.empty();
    const std::string executionOutput =
        result.executionSummary.empty() && hasToolExecution
            ? std::string("Tool execution completed.")
            : result.executionSummary;
    result.output = preferLlmOutput ? modelTextOutput : executionOutput;
    result.outputSource = preferLlmOutput ? "llm_output"
                                          : (hasToolExecution ? "tool_execution"
                                                              : "loop_report");
    if (!report.success) {
      result.errorMessage = result.executionSummary.empty()
                                ? "UltraLoop execution failed."
                                : result.executionSummary;
    }

    if (!report.success && failureTraces.empty()) {
      cognitive::FailureTrace trace;
      trace.iteration = report.iterations;
      trace.phase = cognitive::toString(report.terminalState);
      trace.module = "UltraLoop";
      trace.errorType = "terminal_failure";
      trace.rootCause = collapseFailureText(
          report.message.empty() ? "UltraLoop execution failed." : report.message,
          false);
      failureTraces.push_back(std::move(trace));
    }
  } catch (const std::exception& ex) {
    result.ok = false;
    result.verifyStatus = "FAIL";
    result.confidence = "low";
    result.executionSummary = ex.what();
    result.output = result.executionSummary;
    result.outputSource = "runtime_error";
    result.errorMessage = ex.what();
    failureTraces.push_back({
        0U,
        "RUNTIME",
        "CognitiveRuntime",
        "exception",
        collapseFailureText(ex.what(), false),
        {},
    });
  } catch (...) {
    result.ok = false;
    result.verifyStatus = "FAIL";
    result.confidence = "low";
    result.executionSummary = "Cognitive runtime failed with an unknown error.";
    result.output = result.executionSummary;
    result.outputSource = "runtime_error";
    result.errorMessage = "Cognitive runtime failed with an unknown error.";
    failureTraces.push_back({
        0U,
        "RUNTIME",
        "CognitiveRuntime",
        "exception",
        "Cognitive runtime failed with an unknown error.",
        {},
    });
  }

  finalizeResult();
  return result;
}

}  // namespace ultra::runtime



