#include "ultra_loop.h"
#include "failure_recovery.h"
#include "../intent/IntentRuntime.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

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

[[nodiscard]] std::string joinTaskIds(const std::vector<std::string>& taskIds) {
  if (taskIds.empty()) {
    return "<none>";
  }

  std::string joined;
  for (std::size_t index = 0U; index < taskIds.size(); ++index) {
    if (index != 0U) {
      joined += ", ";
    }
    joined += taskIds[index];
  }
  return joined;
}

[[nodiscard]] std::string joinMessages(const std::vector<std::string>& messages) {
  if (messages.empty()) {
    return {};
  }

  std::ostringstream stream;
  for (std::size_t index = 0U; index < messages.size(); ++index) {
    if (index != 0U) {
      stream << "; ";
    }
    stream << messages[index];
  }
  return stream.str();
}

void pushUnique(std::vector<std::string>& messages, std::string message) {
  if (message.empty()) {
    return;
  }
  if (std::find(messages.begin(), messages.end(), message) == messages.end()) {
    messages.push_back(std::move(message));
  }
}

[[nodiscard]] std::string normalizeKey(const std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());

  bool previousSeparator = false;
  for (const char ch : value) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0) {
      normalized.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      previousSeparator = false;
      continue;
    }

    if (!normalized.empty() && !previousSeparator) {
      normalized.push_back('/');
    }
    previousSeparator = true;
  }

  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }

  return normalized;
}

[[nodiscard]] std::string defaultIntentTarget(const UltraLoopFrame& frame) {
  if (frame.hasStructuredIntent && !frame.structuredIntent.goal.target.empty()) {
    return frame.structuredIntent.goal.target;
  }
  if (!frame.intentTarget.empty()) {
    return frame.intentTarget;
  }
  if (!frame.intentGoal.empty()) {
    return frame.intentGoal;
  }
  return "workspace_root";
}

[[nodiscard]] ::ultra::runtime::intent::Intent buildIntentSnapshot(
    const UltraLoopFrame& frame,
    const bool preferOriginal) {
  const std::size_t fallbackBudget =
      frame.intentTokenBudget == 0U ? 4096U : frame.intentTokenBudget;

  if (preferOriginal && frame.hasOriginalStructuredIntent) {
    return ::ultra::runtime::intent::normalizeIntent(
        frame.originalStructuredIntent,
        frame.originalStructuredIntent.constraints.tokenBudget == 0U
            ? fallbackBudget
            : frame.originalStructuredIntent.constraints.tokenBudget);
  }

  if (frame.hasStructuredIntent) {
    return ::ultra::runtime::intent::normalizeIntent(
        frame.structuredIntent,
        frame.structuredIntent.constraints.tokenBudget == 0U
            ? fallbackBudget
            : frame.structuredIntent.constraints.tokenBudget);
  }

  ::ultra::runtime::intent::Intent intentValue;
  intentValue.goal.type = ::ultra::runtime::intent::GoalType::ModifySymbol;
  intentValue.goal.target = defaultIntentTarget(frame);
  intentValue.constraints.maxImpactDepth =
      std::max<std::size_t>(1U, frame.intentImpactDepth);
  intentValue.constraints.maxFilesChanged =
      std::max<std::size_t>(1U, frame.intentMaxFilesChanged);
  intentValue.constraints.tokenBudget = fallbackBudget;
  intentValue.constraints.branchScope = frame.intentBranchId;
  intentValue.constraints.determinismRequired = true;
  intentValue.risk = frame.intentTolerance;
  intentValue.options.allowPublicAPIChange = frame.intentAllowPublicApiChange;
  return ::ultra::runtime::intent::normalizeIntent(intentValue, fallbackBudget);
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

[[nodiscard]] ::ultra::runtime::intent::Action actionFromPayload(
    const TaskPayload& payload,
    const UltraLoopFrame& frame) {
  if (payload.plannedAction.has_value()) {
    return *payload.plannedAction;
  }

  ::ultra::runtime::intent::Action action;
  action.kind = payload.kind == TaskPayloadKind::Action
                    ? actionKindFromActionType(payload.action.type)
                    : ::ultra::runtime::intent::ActionKind::ModifySymbolBody;
  action.target = payload.kind == TaskPayloadKind::Action
                      ? payload.action.target
                      : payload.intent.goal.target;
  if (action.target.empty()) {
    action.target = defaultIntentTarget(frame);
  }
  action.details = "Inferred deterministic action from runtime payload.";
  action.estimatedFilesChanged = 1U;
  action.estimatedDependencyDepth = 1U;
  action.publicApiSurface = frame.intentAllowPublicApiChange;
  return action;
}

[[nodiscard]] bool actionCompatibleWithGoal(
    const ::ultra::runtime::intent::GoalType goalType,
    const ::ultra::runtime::intent::ActionKind actionKind) noexcept {
  using GoalType = ::ultra::runtime::intent::GoalType;
  using ActionKind = ::ultra::runtime::intent::ActionKind;

  switch (goalType) {
    case GoalType::ModifySymbol:
      return actionKind == ActionKind::ModifySymbolBody ||
             actionKind == ActionKind::RenameSymbol ||
             actionKind == ActionKind::ChangeSignature ||
             actionKind == ActionKind::UpdatePublicAPI;
    case GoalType::RefactorModule:
      return actionKind == ActionKind::RefactorModule ||
             actionKind == ActionKind::ModifySymbolBody ||
             actionKind == ActionKind::RenameSymbol ||
             actionKind == ActionKind::ChangeSignature ||
             actionKind == ActionKind::UpdatePublicAPI ||
             actionKind == ActionKind::MoveAcrossModules;
    case GoalType::ReduceImpactRadius:
      return actionKind == ActionKind::ReduceImpactRadius;
    case GoalType::ImproveCentrality:
      return actionKind == ActionKind::ImproveCentrality ||
             actionKind == ActionKind::ReduceImpactRadius;
    case GoalType::MinimizeTokenUsage:
      return actionKind == ActionKind::MinimizeTokenUsage ||
             actionKind == ActionKind::ReduceImpactRadius ||
             actionKind == ActionKind::ImproveCentrality;
    case GoalType::AddDependency:
      return actionKind == ActionKind::AddDependency;
    case GoalType::RemoveDependency:
      return actionKind == ActionKind::RemoveDependency;
  }
  return false;
}

[[nodiscard]] std::vector<std::string> actionViolations(
    const ::ultra::runtime::intent::Action& action,
    const ::ultra::runtime::intent::Intent& originalIntent) {
  std::vector<std::string> violations;

  if (!actionCompatibleWithGoal(originalIntent.goal.type, action.kind)) {
    pushUnique(violations,
               "Action kind " + ::ultra::runtime::intent::toString(action.kind) +
                   " diverges from goal " +
                   ::ultra::runtime::intent::toString(originalIntent.goal.type) +
                   ".");
  }

  if (!originalIntent.goal.target.empty() &&
      normalizeKey(action.target) != normalizeKey(originalIntent.goal.target)) {
    pushUnique(violations,
               "Action target " + action.target +
                   " diverges from original intent target " +
                   originalIntent.goal.target + ".");
  }

  if (action.estimatedFilesChanged > originalIntent.constraints.maxFilesChanged) {
    pushUnique(violations,
               "Action estimated_files_changed exceeds original limit.");
  }

  if (action.estimatedDependencyDepth >
      originalIntent.constraints.maxImpactDepth) {
    pushUnique(violations,
               "Action estimated_dependency_depth exceeds original limit.");
  }

  if (action.publicApiSurface &&
      !originalIntent.options.allowPublicAPIChange) {
    pushUnique(violations,
               "Action would touch public API outside the original intent options.");
  }

  if (action.kind == ::ultra::runtime::intent::ActionKind::RenameSymbol &&
      !originalIntent.options.allowRename) {
    pushUnique(violations,
               "Rename action is outside the original intent options.");
  }

  if (action.kind == ::ultra::runtime::intent::ActionKind::ChangeSignature &&
      !originalIntent.options.allowSignatureChange) {
    pushUnique(violations,
               "Signature change is outside the original intent options.");
  }

  if (action.kind == ::ultra::runtime::intent::ActionKind::MoveAcrossModules &&
      !originalIntent.options.allowCrossModuleMove) {
    pushUnique(violations,
               "Cross-module move is outside the original intent options.");
  }

  if (action.kind == ::ultra::runtime::intent::ActionKind::UpdatePublicAPI &&
      !originalIntent.options.allowPublicAPIChange) {
    pushUnique(violations,
               "Public API update is outside the original intent options.");
  }

  return violations;
}

struct ArbitrationCandidate {
  std::size_t index{0U};
  TaskPayload payload{};
  ::ultra::runtime::intent::Action action{};
  std::string targetKey;
  std::string taskId;
  long long score{0LL};
};

[[nodiscard]] long long scoreCandidate(
    const ArbitrationCandidate& candidate,
    const ::ultra::runtime::intent::Intent& originalIntent) {
  long long score = 0LL;

  const std::string originalTargetKey = normalizeKey(originalIntent.goal.target);
  const bool targetAligned = originalTargetKey.empty() ||
                             candidate.targetKey == originalTargetKey;
  score += targetAligned ? 40LL : -25LL;

  score += actionCompatibleWithGoal(originalIntent.goal.type, candidate.action.kind)
               ? 35LL
               : -40LL;

  const std::size_t estimatedFiles =
      std::max<std::size_t>(1U, candidate.action.estimatedFilesChanged);
  if (estimatedFiles <= originalIntent.constraints.maxFilesChanged) {
    score += 20LL + static_cast<long long>(
                        originalIntent.constraints.maxFilesChanged - estimatedFiles);
  } else {
    score -= 30LL * static_cast<long long>(
                        estimatedFiles - originalIntent.constraints.maxFilesChanged);
  }

  const std::size_t estimatedDepth =
      std::max<std::size_t>(1U, candidate.action.estimatedDependencyDepth);
  if (estimatedDepth <= originalIntent.constraints.maxImpactDepth) {
    score += 20LL + static_cast<long long>(
                        originalIntent.constraints.maxImpactDepth - estimatedDepth);
  } else {
    score -= 25LL * static_cast<long long>(
                        estimatedDepth - originalIntent.constraints.maxImpactDepth);
  }

  if (candidate.action.publicApiSurface) {
    score += originalIntent.options.allowPublicAPIChange ? 5LL : -50LL;
  }
  if (candidate.action.kind ==
      ::ultra::runtime::intent::ActionKind::RenameSymbol) {
    score += originalIntent.options.allowRename ? 5LL : -30LL;
  }
  if (candidate.action.kind ==
      ::ultra::runtime::intent::ActionKind::ChangeSignature) {
    score += originalIntent.options.allowSignatureChange ? 5LL : -30LL;
  }
  if (candidate.action.kind ==
      ::ultra::runtime::intent::ActionKind::MoveAcrossModules) {
    score += originalIntent.options.allowCrossModuleMove ? 5LL : -30LL;
  }

  const double boundedRisk = std::clamp(candidate.payload.action.riskScore, 0.0, 1.0);
  score += static_cast<long long>((1.0 - boundedRisk) * 10.0);
  return score;
}

[[nodiscard]] bool betterCandidate(const ArbitrationCandidate& left,
                                   const ArbitrationCandidate& right) {
  if (left.score != right.score) {
    return left.score > right.score;
  }
  if (left.action.publicApiSurface != right.action.publicApiSurface) {
    return !left.action.publicApiSurface;
  }
  if (left.action.estimatedFilesChanged != right.action.estimatedFilesChanged) {
    return left.action.estimatedFilesChanged < right.action.estimatedFilesChanged;
  }
  if (left.action.estimatedDependencyDepth !=
      right.action.estimatedDependencyDepth) {
    return left.action.estimatedDependencyDepth <
           right.action.estimatedDependencyDepth;
  }
  if (left.taskId != right.taskId) {
    return left.taskId < right.taskId;
  }
  return left.index < right.index;
}

[[nodiscard]] bool repairDecisionRequestsRetry(
    const std::string_view decision) noexcept {
  return decision == "RETRY_TASK" || decision == "VERIFY_RETRY";
}

}  // namespace

const char* toString(const UltraLoopState state) noexcept {
  switch (state) {
    case UltraLoopState::INIT:
      return "INIT";
    case UltraLoopState::PLAN:
      return "PLAN";
    case UltraLoopState::ARBITRATION:
      return "ARBITRATION";
    case UltraLoopState::MICRO_PLAN:
      return "MICRO_PLAN";
    case UltraLoopState::EXECUTE:
      return "EXECUTE";
    case UltraLoopState::PARTIAL_REPAIR:
      return "PARTIAL_REPAIR";
    case UltraLoopState::VERIFY:
      return "VERIFY";
    case UltraLoopState::REFLECT:
      return "REFLECT";
    case UltraLoopState::RE_ANCHOR:
      return "RE_ANCHOR";
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

IntentConsistencyRecord evaluateIntentConsistency(const UltraLoopFrame& frame) {
  IntentConsistencyRecord record;
  record.iteration = frame.iteration;
  record.planId = frame.planId;

  const ::ultra::runtime::intent::Intent originalIntent =
      buildIntentSnapshot(frame, true);
  const ::ultra::runtime::intent::Intent currentIntent =
      buildIntentSnapshot(frame, false);

  std::vector<std::string> reasons;
  if (currentIntent.goal.type != originalIntent.goal.type) {
    pushUnique(reasons,
               "Structured intent goal type diverged from the original intent.");
  }
  if (normalizeKey(currentIntent.goal.target) !=
      normalizeKey(originalIntent.goal.target)) {
    pushUnique(reasons,
               "Structured intent target diverged from the original intent.");
  }
  if (currentIntent.constraints.maxFilesChanged >
      originalIntent.constraints.maxFilesChanged) {
    pushUnique(reasons,
               "Structured intent max_files_changed drifted beyond the original constraint.");
  }
  if (currentIntent.constraints.maxImpactDepth >
      originalIntent.constraints.maxImpactDepth) {
    pushUnique(reasons,
               "Structured intent max_impact_depth drifted beyond the original constraint.");
  }
  if (currentIntent.options.allowPublicAPIChange &&
      !originalIntent.options.allowPublicAPIChange) {
    pushUnique(reasons,
               "Structured intent expanded public API permissions beyond the original intent.");
  }
  if (currentIntent.options.allowRename && !originalIntent.options.allowRename) {
    pushUnique(reasons,
               "Structured intent expanded rename permissions beyond the original intent.");
  }
  if (currentIntent.options.allowSignatureChange &&
      !originalIntent.options.allowSignatureChange) {
    pushUnique(reasons,
               "Structured intent expanded signature permissions beyond the original intent.");
  }
  if (currentIntent.options.allowCrossModuleMove &&
      !originalIntent.options.allowCrossModuleMove) {
    pushUnique(reasons,
               "Structured intent expanded cross-module move permissions beyond the original intent.");
  }

  for (const TaskPayload& payload : frame.microTaskPayloads) {
    if (payload.kind != TaskPayloadKind::Action) {
      continue;
    }

    const ::ultra::runtime::intent::Action action = actionFromPayload(payload, frame);
    for (std::string violation : actionViolations(action, originalIntent)) {
      pushUnique(reasons, std::move(violation));
    }
  }

  record.consistent = reasons.empty();
  record.reason = record.consistent
                      ? "Plan remains anchored to the original intent."
                      : joinMessages(reasons);
  return record;
}

StageResult DeterministicArbitrationStage::run(UltraLoopFrame& frame) {
  StageResult result;
  ArbitrationDecisionRecord record;
  record.iteration = frame.iteration;
  record.candidateCount = frame.microTaskPayloads.size();

  if (frame.microTaskPayloads.empty()) {
    result.success = false;
    result.signal = StageSignal::Replan;
    result.message = "Arbitration requires candidate micro-actions.";
    record.reason = result.message;
    frame.arbitrationLog.push_back(record);
    return result;
  }

  const ::ultra::runtime::intent::Intent originalIntent =
      buildIntentSnapshot(frame, true);
  std::vector<ArbitrationCandidate> actionCandidates;
  std::vector<std::pair<std::size_t, TaskPayload>> retained;
  actionCandidates.reserve(frame.microTaskPayloads.size());
  retained.reserve(frame.microTaskPayloads.size());

  for (std::size_t index = 0U; index < frame.microTaskPayloads.size(); ++index) {
    const TaskPayload& payload = frame.microTaskPayloads[index];
    if (payload.kind != TaskPayloadKind::Action) {
      retained.push_back({index, payload});
      continue;
    }

    ArbitrationCandidate candidate;
    candidate.index = index;
    candidate.payload = payload;
    candidate.action = actionFromPayload(payload, frame);
    if (candidate.action.target.empty()) {
      candidate.action.target = defaultIntentTarget(frame);
    }
    candidate.targetKey = normalizeKey(candidate.action.target);
    if (candidate.targetKey.empty()) {
      candidate.targetKey = "workspace_root";
    }
    candidate.taskId = payload.action.id.empty()
                           ? candidate.action.target
                           : payload.action.id;
    if (candidate.taskId.empty()) {
      candidate.taskId = "candidate_" + std::to_string(index + 1U);
    }
    candidate.score = scoreCandidate(candidate, originalIntent);
    actionCandidates.push_back(std::move(candidate));
  }

  if (actionCandidates.empty()) {
    record.selectedCount = retained.size();
    record.reason = "Arbitration found no executable action candidates.";
    frame.arbitrationLog.push_back(record);
    result.success = true;
    result.signal = StageSignal::Continue;
    result.message = record.reason;
    return result;
  }

  std::vector<std::string> groupOrder;
  std::vector<std::vector<ArbitrationCandidate>> groups;
  for (ArbitrationCandidate& candidate : actionCandidates) {
    auto orderIt = std::find(groupOrder.begin(), groupOrder.end(), candidate.targetKey);
    if (orderIt == groupOrder.end()) {
      groupOrder.push_back(candidate.targetKey);
      groups.push_back({std::move(candidate)});
      continue;
    }

    const std::size_t groupIndex =
        static_cast<std::size_t>(std::distance(groupOrder.begin(), orderIt));
    groups[groupIndex].push_back(std::move(candidate));
  }

  for (auto& group : groups) {
    if (group.size() > 1U) {
      ++record.conflictCount;
      std::stable_sort(group.begin(), group.end(), betterCandidate);
    }

    ArbitrationCandidate& selected = group.front();
    retained.push_back({selected.index, std::move(selected.payload)});
    record.selectedTaskIds.push_back(selected.taskId);
  }

  std::sort(retained.begin(), retained.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  frame.microTaskPayloads.clear();
  frame.microTaskPayloads.reserve(retained.size());
  for (auto& retainedPayload : retained) {
    frame.microTaskPayloads.push_back(std::move(retainedPayload.second));
  }

  record.selectedCount = frame.microTaskPayloads.size();
  record.reason = record.conflictCount == 0U
                      ? "Arbitration found no conflicting candidate actions."
                      : "Arbitration deterministically selected the best action per conflicting target.";
  frame.arbitrationLog.push_back(record);

  std::cerr << "[UltraLoop][Arbitration] iteration=" << record.iteration
            << " candidates=" << record.candidateCount
            << " conflicts=" << record.conflictCount
            << " selected=" << record.selectedCount
            << " tasks=" << joinTaskIds(record.selectedTaskIds) << "\n";

  result.success = true;
  result.signal = StageSignal::Continue;
  result.message = record.reason;
  return result;
}

StageResult DeterministicReanchorStage::run(UltraLoopFrame& frame) {
  if (!frame.reanchorRequested) {
    return {
        true,
        StageSignal::Continue,
        "Re-anchor stage bypassed; no drift detected.",
    };
  }

  if (!frame.hasOriginalStructuredIntent) {
    return {
        false,
        StageSignal::Replan,
        "Re-anchor stage requires the original structured intent.",
    };
  }

  frame.structuredIntent = frame.originalStructuredIntent;
  frame.hasStructuredIntent = true;
  frame.intentGoal = frame.structuredIntent.goal.target;
  frame.intentTarget = frame.structuredIntent.goal.target;
  frame.intentBranchId = frame.structuredIntent.constraints.branchScope;
  frame.intentTokenBudget =
      std::max<std::size_t>(1U, frame.structuredIntent.constraints.tokenBudget);
  frame.intentImpactDepth = std::max<std::size_t>(
      1U, frame.structuredIntent.constraints.maxImpactDepth);
  frame.intentMaxFilesChanged = std::max<std::size_t>(
      1U, frame.structuredIntent.constraints.maxFilesChanged);
  frame.intentTolerance = frame.structuredIntent.risk;
  frame.intentAllowPublicApiChange =
      frame.structuredIntent.options.allowPublicAPIChange;
  frame.intentId = ::ultra::runtime::intent::toString(frame.structuredIntent.goal.type);
  if (!frame.intentTarget.empty()) {
    frame.intentId += ":" + frame.intentTarget;
  }

  frame.strategyId.clear();
  frame.planId.clear();
  frame.microTaskPayloads.clear();
  frame.hasTaskGraph = false;
  frame.taskGraph = TaskGraph{};
  frame.hasExecutionResult = false;
  frame.executionResult = {};
  frame.executionId.clear();
  frame.replanRequested = false;
  frame.reanchorRequested = false;
  frame.retryCount = 0U;
  frame.intentConsistent = true;
  frame.intentConsistencyReason = "Plan reset to original intent.";

  return {
      true,
      StageSignal::Continue,
      "Re-anchor stage restored the original structured intent and discarded drifted plan state.",
  };
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
    const UltraLoopState currentState = state;
    const std::string logReason = reason;
    std::cerr << "[UltraLoop][Transition] from=" << toString(currentState)
              << " to=" << toString(nextState)
              << " reason=" << logReason << "\n";
    report.transitions.push_back({currentState, nextState, std::move(reason)});
    state = nextState;
  };

  auto notifyFailure = [&](const StageResult& result) {
    frame.failureDetected = true;
    if (config_.failureHook) {
      config_.failureHook(state, result, frame);
    }
  };

  auto clearRepairContext = [&]() {
    frame.repairSite.clear();
    frame.repairTaskIds.clear();
    frame.repairDecision.clear();
    frame.repairReason.clear();
  };

  auto schedulePartialRepair = [&](std::string site,
                                   std::vector<std::string> taskIds,
                                   std::string decision,
                                   std::string reason) {
    frame.repairSite = std::move(site);
    frame.repairTaskIds = std::move(taskIds);
    frame.repairDecision = std::move(decision);
    frame.repairReason = std::move(reason);
    transitionTo(UltraLoopState::PARTIAL_REPAIR,
                 "Scheduled partial repair for " + frame.repairSite +
                     " tasks=" + joinTaskIds(frame.repairTaskIds) + ".");
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

    if (frame.hasStructuredIntent) {
      if (frame.intentGoal.empty()) {
        frame.intentGoal = frame.structuredIntent.goal.target;
      }
      if (frame.intentTarget.empty()) {
        frame.intentTarget = frame.structuredIntent.goal.target.empty()
                                 ? frame.intentGoal
                                 : frame.structuredIntent.goal.target;
      }
      if (frame.intentId.empty()) {
        frame.intentId =
            ::ultra::runtime::intent::toString(frame.structuredIntent.goal.type);
        if (!frame.intentTarget.empty()) {
          frame.intentId += ":" + frame.intentTarget;
        }
      }
      if (!frame.hasOriginalStructuredIntent) {
        frame.originalStructuredIntent = frame.structuredIntent;
        frame.hasOriginalStructuredIntent = true;
        frame.originalIntentId = frame.intentId;
      }

      result.success = true;
      result.signal = StageSignal::Continue;
      result.message =
          "Structured intent already supplied; skipping re-resolution.";
      return result;
    }

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
      if (!frame.hasOriginalStructuredIntent) {
        frame.originalStructuredIntent = frame.structuredIntent;
        frame.hasOriginalStructuredIntent = true;
        frame.originalIntentId = frame.intentId;
      }

      result.success = true;
      result.signal = StageSignal::Continue;
      result.message =
          "Intent runtime produced a structured deterministic intent.";
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
        frame.executionId = taskNode.id;
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
    if (!frame.executionResult.text_output.empty()) {
      std::cerr << "[UltraLoop] Task " << frame.executionId
                << " produced text_output bytes="
                << frame.executionResult.text_output.size() << "\n";
    }

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
        if (frame.retryCount < config_.maxRetriesPerIteration) {
          schedulePartialRepair("EXECUTE",
                                {failedTask.id},
                                toString(action),
                                failureContext.execution_result.message.empty()
                                    ? failureResult.message
                                    : failureContext.execution_result.message);
          return;
        }
        frame.replanRequested = true;
        transitionTo(UltraLoopState::REPLAN,
                     "Failure recovery could not retry task; replanning.");
        return;

      case RecoveryAction::SKIP_TASK:
        schedulePartialRepair("EXECUTE",
                              {failedTask.id},
                              toString(action),
                              failureContext.execution_result.message.empty()
                                  ? failureResult.message
                                  : failureContext.execution_result.message);
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
        frame.reanchorRequested = false;
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
        clearRepairContext();
        frame.intentConsistent = true;
        frame.intentConsistencyReason.clear();

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

        transitionTo(UltraLoopState::ARBITRATION,
                     "Planning stages completed with structured intent.");
        break;
      }

      case UltraLoopState::ARBITRATION: {
        const StageResult arbitrationResult = bindings.arbitration->run(frame);
        if (handleTerminalSignal("Arbitration", arbitrationResult)) {
          break;
        }
        if (!arbitrationResult.success) {
          notifyFailure(arbitrationResult);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Arbitration stage failed; triggering replanning.");
          break;
        }
        if (arbitrationResult.signal != StageSignal::Continue) {
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Arbitration stage requested replanning.");
          break;
        }

        transitionTo(UltraLoopState::MICRO_PLAN,
                     "Arbitration selected a deterministic execution path.");
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
            schedulePartialRepair("EXECUTE",
                                  failedTasks,
                                  "RETRY_TASK",
                                  "Execute stage found failed tasks with remaining retry budget.");
            break;
          }

          const std::string blockedDebug =
              "Blocked tasks: " +
              (failedTasks.empty() ? std::string("<none failed>")
                                   : joinTaskIds(failedTasks));
          if (!report.message.empty()) {
            report.message += " | ";
          }
          report.message += blockedDebug;
          std::cerr << "[UltraLoop] " << blockedDebug << "\n";
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "No ready tasks available while work remains. " +
                           blockedDebug);
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

      case UltraLoopState::PARTIAL_REPAIR: {
        if (!frame.hasTaskGraph) {
          StageResult missingGraph;
          missingGraph.success = false;
          missingGraph.signal = StageSignal::Replan;
          missingGraph.message =
              "Partial repair requires an initialized TaskGraph.";
          notifyFailure(missingGraph);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN, missingGraph.message);
          break;
        }

        if (frame.repairTaskIds.empty()) {
          StageResult missingTasks;
          missingTasks.success = false;
          missingTasks.signal = StageSignal::Replan;
          missingTasks.message =
              "Partial repair requires failed or drifted task identifiers.";
          notifyFailure(missingTasks);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN, missingTasks.message);
          break;
        }

        RepairRecord record;
        record.iteration = frame.iteration;
        record.site = frame.repairSite.empty() ? "UNKNOWN" : frame.repairSite;
        record.taskIds = frame.repairTaskIds;
        record.decision =
            frame.repairDecision.empty() ? "RETRY_TASK" : frame.repairDecision;
        record.reason = frame.repairReason.empty()
                            ? "Partial repair requested deterministic recovery."
                            : frame.repairReason;

        bool repaired = false;
        const bool shouldRetry = repairDecisionRequestsRetry(record.decision);
        if (shouldRetry) {
          if (frame.retryCount >= config_.maxRetriesPerIteration) {
            frame.replanRequested = true;
            transitionTo(UltraLoopState::REPLAN,
                         "Partial repair retry budget exhausted.");
            break;
          }

          for (const std::string& taskId : frame.repairTaskIds) {
            repaired = frame.taskGraph.reopen_task(taskId) || repaired;
          }
          if (repaired) {
            ++frame.retryCount;
            ++report.retries;
          }
          record.attempt = frame.retryCount;
        } else if (record.decision == "SKIP_TASK") {
          for (const std::string& taskId : frame.repairTaskIds) {
            bool advanced = frame.taskGraph.reset_failed(taskId);
            if (!advanced) {
              advanced = frame.taskGraph.reopen_task(taskId);
            }
            if (advanced && frame.taskGraph.mark_completed(taskId)) {
              repaired = true;
            }
          }
          record.attempt = frame.retryCount;
        }

        frame.repairLog.push_back(record);
        std::cerr << "[UltraLoop][Repair] iteration=" << record.iteration
                  << " attempt=" << record.attempt
                  << " site=" << record.site
                  << " decision=" << record.decision
                  << " tasks=" << joinTaskIds(record.taskIds)
                  << " reason=" << record.reason << "\n";

        if (!repaired) {
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Partial repair could not recover requested tasks.");
          clearRepairContext();
          break;
        }

        frame.hasExecutionResult = false;
        frame.executionResult = {};
        frame.executionId.clear();
        clearRepairContext();
        transitionTo(UltraLoopState::EXECUTE,
                     "Partial repair preserved successful work and reopened targeted tasks.");
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
          std::vector<std::string> repairTasks = frame.taskGraph.failed_tasks();
          if (repairTasks.empty() && !frame.executionId.empty()) {
            repairTasks.push_back(frame.executionId);
          }

          if (!repairTasks.empty()) {
            schedulePartialRepair("VERIFY",
                                  std::move(repairTasks),
                                  "VERIFY_RETRY",
                                  verificationResult.message.empty()
                                      ? "Verification requested deterministic repair retry."
                                      : verificationResult.message);
            break;
          }
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

        if (frame.reanchorRequested) {
          transitionTo(UltraLoopState::RE_ANCHOR,
                       frame.intentConsistencyReason.empty()
                           ? "Reflection detected intent drift."
                           : frame.intentConsistencyReason);
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

      case UltraLoopState::RE_ANCHOR: {
        const StageResult reanchorResult = bindings.reanchor->run(frame);
        if (handleTerminalSignal("Re-anchor", reanchorResult)) {
          break;
        }
        if (!reanchorResult.success) {
          notifyFailure(reanchorResult);
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Re-anchor stage failed; triggering replanning.");
          break;
        }
        if (reanchorResult.signal != StageSignal::Continue) {
          frame.replanRequested = true;
          transitionTo(UltraLoopState::REPLAN,
                       "Re-anchor stage requested replanning.");
          break;
        }

        transitionTo(UltraLoopState::PLAN,
                     "Re-anchor restored the original intent; returning to PLAN.");
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
        report.repairs = frame.repairLog;
        report.arbitration = frame.arbitrationLog;
        report.intentConsistency = frame.intentConsistencyLog;
        running = false;
        break;
      }
    }
  }

  if (report.repairs.empty()) {
    report.repairs = frame.repairLog;
  }
  if (report.arbitration.empty()) {
    report.arbitration = frame.arbitrationLog;
  }
  if (report.intentConsistency.empty()) {
    report.intentConsistency = frame.intentConsistencyLog;
  }

  return report;
}

std::vector<std::string> UltraLoop::detectMissingLayers(
    const UltraLoopBindings& bindings) const {
  std::vector<std::string> missing;

  if (bindings.intent == nullptr) {
    missing.emplace_back("L11 Intent Runtime");
  }
  if (bindings.arbitration == nullptr) {
    missing.emplace_back("Arbitration");
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
  if (bindings.reanchor == nullptr) {
    missing.emplace_back("Re-anchor");
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

