#include "ultra_loop.h"
#include "failure_recovery.h"
#include "../intent/IntentRuntime.h"
#include <algorithm>
#include <utility>

namespace ultra::runtime::cognitive {

namespace {

[[nodiscard]] std::string stageFailureMessage(
    const std::string_view stageName,
    const StageResult& result) {
  if (!result.message.empty()) {
    return result.message;
  }
  return std::string(stageName) + " stage reported failure.";
}

[[nodiscard]] std::string stageTerminationMessage(
    const std::string_view stageName,
    const StageResult& result,
    const bool success) {
  if (!result.message.empty()) {
    return result.message;
  }
  return std::string(stageName) +
         (success ? " stage requested successful termination."
                  : " stage requested failed termination.");
}

}  // namespace

const char* toString(const UltraLoopState state) noexcept {
  switch (state) {
    case UltraLoopState::INIT:
      return "INIT";
    case UltraLoopState::PLAN:
      return "PLAN";
    case UltraLoopState::MICRO_PLAN:
      return "MICRO_PLAN";
    case UltraLoopState::EXECUTE:
      return "EXECUTE";
    case UltraLoopState::VERIFY:
      return "VERIFY";
    case UltraLoopState::REFLECT:
      return "REFLECT";
    case UltraLoopState::REPLAN:
      return "REPLAN";
    case UltraLoopState::TERMINATE:
      return "TERMINATE";
  }
  return "TERMINATE";
}

const char* toString(const StageSignal signal) noexcept {
  switch (signal) {
    case StageSignal::Continue:
      return "Continue";
    case StageSignal::Retry:
      return "Retry";
    case StageSignal::Replan:
      return "Replan";
    case StageSignal::TerminateSuccess:
      return "TerminateSuccess";
    case StageSignal::TerminateFailure:
      return "TerminateFailure";
  }
  return "TerminateFailure";
}

UltraLoop::UltraLoop(UltraLoopConfig config)
    : config_(std::move(config)) {}

UltraLoopReport UltraLoop::run(const UltraLoopBindings& bindings) const {
  UltraLoopReport report;
  UltraLoopFrame frame;
  UltraLoopState state = UltraLoopState::INIT;
  bool memoryInvoked = false;
  bool running = true;
  const FailureRecoveryEngine failureRecoveryEngine{};

  auto transitionTo = [&](const UltraLoopState nextState, std::string reason) {
    report.transitions.push_back({state, nextState, std::move(reason)});
    state = nextState;
  };

  auto notifyFailure = [&](const StageResult& result) {
    frame.failureDetected = true;
    if (config_.failureHook) {
      config_.failureHook(state, result, frame);
    }
  };

  auto handleTerminalSignal = [&](const std::string_view stageName,
                                  const StageResult& result) {
    if (result.signal == StageSignal::TerminateSuccess) {
      report.success = true;
      report.message = stageTerminationMessage(stageName, result, true);
      transitionTo(UltraLoopState::TERMINATE, report.message);
      return true;
    }

    if (result.signal == StageSignal::TerminateFailure) {
      notifyFailure(result);
      report.success = false;
      report.message = stageTerminationMessage(stageName, result, false);
      transitionTo(UltraLoopState::TERMINATE, report.message);
      return true;
    }

    return false;
  };

  auto runMemoryCheckpoint = [&](const std::string_view checkpointName) {
    const StageResult memoryResult = bindings.memory->run(frame);
    memoryInvoked = true;

    if (memoryResult.success && memoryResult.signal == StageSignal::Continue) {
      return true;
    }

    if (memoryResult.signal == StageSignal::TerminateSuccess) {
      report.success = true;
      report.message = stageTerminationMessage("Memory", memoryResult, true);
      transitionTo(UltraLoopState::TERMINATE, report.message);
      return false;
    }

    notifyFailure(memoryResult);
    report.success = false;
    report.message = stageFailureMessage("Memory", memoryResult);
    if (memoryResult.message.empty()) {
      report.message += " Checkpoint=";
      report.message += std::string(checkpointName);
      report.message += ".";
    }
    transitionTo(UltraLoopState::TERMINATE, report.message);
    return false;
  };

  auto resolveStructuredIntent = [&]() {
    StageResult result;

    const ::ultra::runtime::intent::ContextFrame context{
        frame.intentGoal,
        frame.intentTarget,
        frame.intentBranchId,
        frame.intentTokenBudget,
        frame.intentImpactDepth,
        frame.intentMaxFilesChanged,
        frame.intentTolerance,
        frame.intentAllowPublicApiChange,
        frame.intentRiskThreshold,
    };

    const std::string resolvedGoal =
        frame.intentGoal.empty() ? frame.intentId : frame.intentGoal;
    const std::string resolvedTarget =
        frame.intentTarget.empty() ? resolvedGoal : frame.intentTarget;

    try {
      ::ultra::runtime::intent::IntentRuntime intentRuntime;
      frame.structuredIntent =
          intentRuntime.resolve_structured_intent(frame.intentId, context);
      frame.hasStructuredIntent = true;
      frame.intentGoal = resolvedGoal;
      frame.intentTarget = resolvedTarget;
      frame.intentId =
          ::ultra::runtime::intent::toString(frame.structuredIntent.goal.type) +
          ":" + frame.structuredIntent.goal.target;

      result.success = true;
      result.signal = StageSignal::Continue;
      result.message = "Intent runtime produced a structured deterministic intent.";
      return result;
    } catch (const std::exception& ex) {
      result.success = false;
      result.signal = StageSignal::Replan;
      result.message = ex.what();
      return result;
    } catch (...) {
      result.success = false;
      result.signal = StageSignal::Replan;
      result.message = "Intent runtime failed with an unknown error.";
      return result;
    }
  };
  auto executeTaskNode = [&](const TaskNode& taskNode) {
    StageResult executionResult;

    if (bindings.executionKernel == nullptr) {
      executionResult.success = false;
      executionResult.signal = StageSignal::TerminateFailure;
      executionResult.message = "Execution kernel binding is missing.";
      return executionResult;
    }

    if (frame.cognitiveState == nullptr) {
      executionResult.success = false;
      executionResult.signal = StageSignal::Replan;
      executionResult.message =
          "Task graph execution is missing a pinned cognitive state.";
      return executionResult;
    }

    const ::ultra::runtime::CognitiveState& stateRef = *frame.cognitiveState;

    switch (taskNode.payload.kind) {
      case TaskPayloadKind::Action: {
        ::ultra::runtime::Action action = taskNode.payload.action;
        if (action.id.empty()) {
          action.id = taskNode.id;
        }
        if (action.snapshotVersion == 0U) {
          action.snapshotVersion = stateRef.snapshot.version;
        }
        if (action.branch.empty()) {
          action.branch = stateRef.snapshot.branch.toString();
        }

        frame.executionResult = bindings.executionKernel->execute(action, stateRef);
        frame.executionId = action.id;
        break;
      }

      case TaskPayloadKind::Intent:
        frame.executionResult = bindings.executionKernel->executeIntent(
            taskNode.payload.intent,
            stateRef,
            taskNode.payload.policy);
        frame.executionId = taskNode.id;
        break;
    }

    frame.hasExecutionResult = true;

    if (frame.executionResult.ok && !frame.executionResult.rolledBack) {
      executionResult.success = true;
      executionResult.signal = StageSignal::Continue;
      executionResult.message = frame.executionResult.message;
      return executionResult;
    }

    executionResult.success = false;
    executionResult.message = frame.executionResult.message.empty()
                                  ? "Execution kernel reported failure."
                                  : frame.executionResult.message;
    if (executionResult.message.find("Governance blocked") != std::string::npos) {
      executionResult.signal = StageSignal::Replan;
      return executionResult;
    }

    executionResult.signal = StageSignal::Retry;
    return executionResult;
  };

  auto dispatchRecovery = [&](const std::string_view failureSite) {
    const StageResult recoveryResult = bindings.recovery->run(frame);

    if (handleTerminalSignal("Recovery", recoveryResult)) {
      return;
    }

    if (!recoveryResult.success) {
      notifyFailure(recoveryResult);
      report.success = false;
      report.message = stageFailureMessage("Recovery", recoveryResult);
      transitionTo(UltraLoopState::TERMINATE, report.message);
      return;
    }

    if (recoveryResult.signal == StageSignal::Retry) {
      if (frame.retryCount < config_.maxRetriesPerIteration) {
        for (const std::string& failedTask : frame.taskGraph.failed_tasks()) {
          (void)frame.taskGraph.reset_failed(failedTask);
        }

        ++frame.retryCount;
        ++report.retries;
        transitionTo(
            UltraLoopState::EXECUTE,
            "Recovery requested retry after " + std::string(failureSite) +
                " failure.");
        return;
      }

      frame.replanRequested = true;
      transitionTo(
          UltraLoopState::REPLAN,
          "Recovery retry budget exhausted after " + std::string(failureSite) +
              " failure.");
      return;
    }

    frame.replanRequested = true;
    transitionTo(UltraLoopState::REPLAN,
                 "Recovery redirected loop to REPLAN state.");
  };

  auto applyFailureRecoveryDecision = [&](const TaskNode& failedTask,
                                          const StageResult& failureResult) {
    FailureContext failureContext;
    failureContext.task_id = failedTask.id;
    failureContext.execution_result = frame.hasExecutionResult
                                          ? frame.executionResult
                                          : ::ultra::runtime::Result{};
    if (!frame.hasExecutionResult && !failureResult.message.empty()) {
      failureContext.execution_result.ok = false;
      failureContext.execution_result.rolledBack = false;
      failureContext.execution_result.message = failureResult.message;
    }
    failureContext.retry_count = frame.retryCount;
    failureContext.retry_limit = config_.maxRetriesPerIteration;
    failureContext.dependency_state = DependencyState{
        frame.taskGraph.has_pending_tasks(),
        frame.taskGraph.failed_tasks().size(),
    };

    const RecoveryAction action = failureRecoveryEngine.decide(failureContext);

    switch (action) {
      case RecoveryAction::RETRY_TASK:
        if (frame.retryCount < config_.maxRetriesPerIteration &&
            frame.taskGraph.reset_failed(failedTask.id)) {
          ++frame.retryCount;
          ++report.retries;
          transitionTo(UltraLoopState::EXECUTE,
                       "Failure recovery requested RETRY_TASK for " +
                           failedTask.id + ".");
          return;
        }
        frame.replanRequested = true;
        transitionTo(UltraLoopState::REPLAN,
                     "Failure recovery could not retry task; replanning.");
        return;

      case RecoveryAction::SKIP_TASK:
        if (frame.taskGraph.reset_failed(failedTask.id) &&
            frame.taskGraph.mark_completed(failedTask.id)) {
          transitionTo(UltraLoopState::EXECUTE,
                       "Failure recovery skipped task " + failedTask.id + ".");
          return;
        }
        frame.replanRequested = true;
        transitionTo(UltraLoopState::REPLAN,
                     "Failure recovery could not skip task safely; replanning.");
        return;

      case RecoveryAction::REPLAN_REQUIRED:
        frame.replanRequested = true;
        transitionTo(UltraLoopState::REPLAN,
                     "Failure recovery requested REPLAN_REQUIRED for " +
                         failedTask.id + ".");
        return;

      case RecoveryAction::ABORT_LOOP:
        report.success = false;
        report.message = stageFailureMessage("Execution Kernel", failureResult);
        transitionTo(UltraLoopState::TERMINATE,
                     "Failure recovery requested ABORT_LOOP for " +
                         failedTask.id + ".");
        return;
    }
  };

  while (running) {
    switch (state) {
      case UltraLoopState::INIT: {
        report.missingLayers = detectMissingLayers(bindings);
        if (!report.missingLayers.empty()) {
          report.success = false;
          report.message = "Missing required layer bindings.";
          transitionTo(UltraLoopState::TERMINATE, report.message);
          break;
        }

        if (config_.maxIterations == 0U) {
          report.success = false;
          report.terminatedByIterationCap = true;
          report.message = "Loop configuration requires maxIterations > 0.";
          transitionTo(UltraLoopState::TERMINATE, report.message);
          break;
        }

        transitionTo(UltraLoopState::PLAN, "Initialization completed.");
        break;
      }

      case UltraLoopState::PLAN: {
        if (report.iterations >= config_.maxIterations) {
          report.success = false;
          report.terminatedByIterationCap = true;
          report.message =
              "Loop reached maxIterations before completing planning.";
          transitionTo(UltraLoopState::TERMINATE, report.message);
          break;
        }

        ++report.iterations;
        frame.iteration = report.iterations;
        frame.retryCount = 0U;
        frame.failureDetected = false;
        frame.verificationPassed = false;
        frame.replanRequested = false;
        frame.cognitiveState = nullptr;

        frame.intentGoal.clear();
        frame.intentTarget.clear();
        frame.intentBranchId.clear();
        frame.intentTokenBudget = 4096U;
        frame.intentImpactDepth = 2U;
        frame.intentMaxFilesChanged = 8U;
        frame.intentTolerance =
            ::ultra::runtime::intent::RiskTolerance::MEDIUM;
        frame.intentAllowPublicApiChange = false;
        frame.intentRiskThreshold = 0.66;
        frame.hasStructuredIntent = false;
        frame.structuredIntent = {};

        frame.intentId.clear();
        frame.strategyId.clear();
        frame.planId = "plan_" + std::to_string(frame.iteration);
        frame.microTaskPayloads.clear();
        frame.hasTaskGraph = false;
        frame.taskGraph = TaskGraph{};
        frame.hasExecutionResult = false;
        frame.executionResult = {};
        frame.executionId.clear();

        const StageResult intentResult = bindings.intent->run(frame);
        if (handleTerminalSignal("Intent", intentResult)) {
          break;
        }
        if (!intentResult.success) {
          notifyFailure(intentResult);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Intent stage failed; triggering replanning.");
          break;
        }
        if (intentResult.signal != StageSignal::Continue) {
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Intent stage requested replanning.");
          break;
        }

        const StageResult intentRuntimeResult = resolveStructuredIntent();
        if (handleTerminalSignal("Intent Runtime", intentRuntimeResult)) {
          break;
        }
        if (!intentRuntimeResult.success ||
            intentRuntimeResult.signal != StageSignal::Continue) {
          if (!intentRuntimeResult.success) {
            notifyFailure(intentRuntimeResult);
          }
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Intent runtime failed to resolve structured intent.");
          break;
        }

        if (bindings.strategy != nullptr) {
          const StageResult strategyResult = bindings.strategy->run(frame);
          if (handleTerminalSignal("Strategy", strategyResult)) {
            break;
          }
          if (!strategyResult.success) {
            notifyFailure(strategyResult);
            frame.replanRequested = true;
            transitionTo(UltraLoopState::REPLAN,
                         "Strategy stage failed; triggering replanning.");
            break;
          }
          if (strategyResult.signal != StageSignal::Continue) {
            frame.replanRequested = true;
            transitionTo(UltraLoopState::REPLAN,
                         "Strategy stage requested replanning.");
            break;
          }
        }


        transitionTo(UltraLoopState::MICRO_PLAN,
                     "Planning stages completed with structured intent.");
        break;
      }

      case UltraLoopState::MICRO_PLAN: {
        if (bindings.microPlanning != nullptr) {
          const StageResult microResult = bindings.microPlanning->run(frame);
          if (handleTerminalSignal("Micro Planning", microResult)) {
            break;
          }
          if (!microResult.success) {
            notifyFailure(microResult);
            frame.replanRequested = true;
            transitionTo(UltraLoopState::REPLAN,
                         "Micro planning stage failed; triggering replanning.");
            break;
          }
          if (microResult.signal != StageSignal::Continue) {
            frame.replanRequested = true;
            transitionTo(UltraLoopState::REPLAN,
                         "Micro planning stage requested replanning.");
            break;
          }
        }

        if (!frame.hasStructuredIntent) {
          StageResult missingIntent;
          missingIntent.success = false;
          missingIntent.signal = StageSignal::Replan;
          missingIntent.message =
              "Micro planner requires a resolved structured intent.";
          notifyFailure(missingIntent);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN, missingIntent.message);
          break;
        }

        std::vector<TaskPayload> plannerPayloads;
        plannerPayloads.reserve(frame.microTaskPayloads.size() + 1U);

        TaskPayload intentPayload;
        intentPayload.kind = TaskPayloadKind::Intent;
        intentPayload.intent = frame.structuredIntent;
        plannerPayloads.push_back(std::move(intentPayload));
        plannerPayloads.insert(plannerPayloads.end(),
                               frame.microTaskPayloads.begin(),
                               frame.microTaskPayloads.end());

        const MicroPlanInput input{
            frame.intentId,
            frame.strategyId,
            frame.planId,
            std::move(plannerPayloads),
        };

        frame.taskGraph = bindings.microPlanner->generate_plan(input);
        frame.hasTaskGraph = !frame.taskGraph.empty();

        if (!frame.hasTaskGraph) {
          StageResult missingGraph;
          missingGraph.success = false;
          missingGraph.signal = StageSignal::Replan;
          missingGraph.message =
              "Micro planner did not produce a valid TaskGraph.";
          notifyFailure(missingGraph);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN, missingGraph.message);
          break;
        }

        transitionTo(UltraLoopState::EXECUTE,
                     "Micro planner produced TaskGraph output.");
        break;
      }

      case UltraLoopState::EXECUTE: {
        const StageResult governanceResult = bindings.governance->run(frame);
        if (handleTerminalSignal("Governance", governanceResult)) {
          break;
        }
        if (!governanceResult.success ||
            governanceResult.signal != StageSignal::Continue) {
          if (!governanceResult.success) {
            notifyFailure(governanceResult);
          }
          dispatchRecovery("governance");
          break;
        }

        if (!frame.hasTaskGraph) {
          StageResult missingGraph;
          missingGraph.success = false;
          missingGraph.signal = StageSignal::Replan;
          missingGraph.message = "Execute stage requires an initialized TaskGraph.";
          notifyFailure(missingGraph);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN, missingGraph.message);
          break;
        }

        if (frame.cognitiveState == nullptr) {
          StageResult missingState;
          missingState.success = false;
          missingState.signal = StageSignal::Replan;
          missingState.message =
              "Execute stage requires a pinned cognitive state.";
          notifyFailure(missingState);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN, missingState.message);
          break;
        }

        const std::vector<TaskNode> readyTasks = frame.taskGraph.get_ready_tasks();
        if (readyTasks.empty()) {
          if (!frame.taskGraph.has_pending_tasks()) {
            transitionTo(UltraLoopState::VERIFY,
                         "All task graph tasks completed.");
            break;
          }

          const std::vector<std::string> failedTasks =
              frame.taskGraph.failed_tasks();
          if (!failedTasks.empty() &&
              frame.retryCount < config_.maxRetriesPerIteration) {
            for (const std::string& failedTask : failedTasks) {
              (void)frame.taskGraph.reset_failed(failedTask);
            }
            ++frame.retryCount;
            ++report.retries;
            transitionTo(UltraLoopState::EXECUTE,
                         "Retrying failed task graph nodes.");
            break;
          }

          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "No ready tasks available while work remains.");
          break;
        }

        bool executeFailed = false;
        for (const TaskNode& readyTask : readyTasks) {
          if (!frame.taskGraph.mark_running(readyTask.id)) {
            StageResult markError;
            markError.success = false;
            markError.signal = StageSignal::Replan;
            markError.message = "Failed to mark task as RUNNING: " + readyTask.id;
            notifyFailure(markError);
            frame.replanRequested = true;
            transitionTo(UltraLoopState::REPLAN, markError.message);
            executeFailed = true;
            break;
          }

          const StageResult executionResult = executeTaskNode(readyTask);
          if (handleTerminalSignal("Execution Kernel", executionResult)) {
            executeFailed = true;
            break;
          }

          if (!executionResult.success ||
              executionResult.signal != StageSignal::Continue) {
            (void)frame.taskGraph.mark_failed(readyTask.id);
            notifyFailure(executionResult);

            if (executionResult.signal == StageSignal::Replan) {
              frame.replanRequested = true;
              transitionTo(UltraLoopState::REPLAN,
                           executionResult.message.empty()
                               ? "Execution kernel requested replanning."
                               : executionResult.message);
              executeFailed = true;
              break;
            }

            applyFailureRecoveryDecision(readyTask, executionResult);
            executeFailed = true;
            break;
          }

          (void)frame.taskGraph.mark_completed(readyTask.id);
        }

        if (executeFailed) {
          break;
        }

        if (frame.taskGraph.has_pending_tasks()) {
          transitionTo(UltraLoopState::EXECUTE,
                       "Executed ready task batch; continuing.");
        } else {
          transitionTo(UltraLoopState::VERIFY,
                       "All task graph tasks completed.");
        }
        break;
      }

      case UltraLoopState::VERIFY: {
        const StageResult verificationResult = bindings.verification->run(frame);
        if (handleTerminalSignal("Verification", verificationResult)) {
          break;
        }
        if (verificationResult.success &&
            verificationResult.signal == StageSignal::Continue) {
          frame.verificationPassed = true;
          transitionTo(UltraLoopState::REFLECT,
                       "Verification stage passed.");
          break;
        }

        if (!verificationResult.success) {
          notifyFailure(verificationResult);
        }

        if (verificationResult.signal == StageSignal::Retry &&
            frame.retryCount < config_.maxRetriesPerIteration) {
          for (const std::string& failedTask : frame.taskGraph.failed_tasks()) {
            (void)frame.taskGraph.reset_failed(failedTask);
          }
          ++frame.retryCount;
          ++report.retries;
          transitionTo(UltraLoopState::EXECUTE,
                       "Verification requested execution retry.");
          break;
        }

        frame.replanRequested = true;
        transitionTo(UltraLoopState::REPLAN,
                     "Verification requested replanning.");
        break;
      }

      case UltraLoopState::REFLECT: {
        const StageResult reflectionResult = bindings.reflection->run(frame);
        if (handleTerminalSignal("Reflection", reflectionResult)) {
          break;
        }
        if (!reflectionResult.success) {
          notifyFailure(reflectionResult);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Reflection stage failed; triggering replanning.");
          break;
        }
        if (reflectionResult.signal != StageSignal::Continue) {
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Reflection stage requested replanning.");
          break;
        }

        if (!runMemoryCheckpoint("REFLECT")) {
          break;
        }

        report.success = true;
        report.message = reflectionResult.message.empty()
                             ? "Loop completed deterministic execution cycle."
                             : reflectionResult.message;
        transitionTo(UltraLoopState::TERMINATE,
                     "Reflection and memory checkpoint completed.");
        break;
      }

      case UltraLoopState::REPLAN: {
        const StageResult replanResult = bindings.replanning->run(frame);
        if (handleTerminalSignal("Replanning", replanResult)) {
          break;
        }
        if (!replanResult.success) {
          notifyFailure(replanResult);
          report.success = false;
          report.message = stageFailureMessage("Replanning", replanResult);
          transitionTo(UltraLoopState::TERMINATE, report.message);
          break;
        }

        if (!runMemoryCheckpoint("REPLAN")) {
          break;
        }

        frame.replanRequested = false;

        if (report.iterations >= config_.maxIterations) {
          report.success = false;
          report.terminatedByIterationCap = true;
          report.message =
              "Loop reached maxIterations while replanning.";
          transitionTo(UltraLoopState::TERMINATE, report.message);
          break;
        }

        transitionTo(UltraLoopState::PLAN,
                     "Replanning completed; returning to PLAN state.");
        break;
      }

      case UltraLoopState::TERMINATE: {
        if (!memoryInvoked) {
          const StageResult memoryResult = bindings.memory->run(frame);
          memoryInvoked = true;

          if (!(memoryResult.success &&
                memoryResult.signal == StageSignal::Continue)) {
            if (memoryResult.signal != StageSignal::TerminateSuccess) {
              notifyFailure(memoryResult);
              report.success = false;
            }

            if (report.message.empty()) {
              report.message = stageFailureMessage("Memory", memoryResult);
            }
          }
        }

        if (report.message.empty()) {
          report.message = report.success
                               ? "Loop terminated successfully."
                               : "Loop terminated with failure.";
        }

        report.terminalState = UltraLoopState::TERMINATE;
        running = false;
        break;
      }
    }
  }

  return report;
}

std::vector<std::string> UltraLoop::detectMissingLayers(
    const UltraLoopBindings& bindings) const {
  std::vector<std::string> missing;

  if (bindings.intent == nullptr) {
    missing.emplace_back("L11 Intent Runtime");
  }
  if (bindings.microPlanner == nullptr) {
    missing.emplace_back("L13 Micro Planning");
  }
  if (bindings.executionKernel == nullptr) {
    missing.emplace_back("L15 Execution Kernel");
  } else {
    if (!bindings.executionKernel->hasToolCognitionLayer()) {
      missing.emplace_back("L16 Tool Cognition Layer");
    }
    if (!bindings.executionKernel->hasToolRouterLayer()) {
      missing.emplace_back("L17 Tool Router");
    }
  }
  if (bindings.governance == nullptr) {
    missing.emplace_back("L18 Governance");
  }
  if (bindings.recovery == nullptr) {
    missing.emplace_back("L19 Recovery");
  }
  if (bindings.verification == nullptr) {
    missing.emplace_back("Verification");
  }
  if (bindings.reflection == nullptr) {
    missing.emplace_back("Reflection");
  }
  if (bindings.replanning == nullptr) {
    missing.emplace_back("Replanning");
  }
  if (bindings.memory == nullptr) {
    missing.emplace_back("L24 Memory");
  }

  return missing;
}

}  // namespace ultra::runtime::cognitive








