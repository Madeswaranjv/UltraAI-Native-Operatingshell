#include "micro_planner.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ultra::runtime::cognitive {

namespace {

std::string normalizeToken(std::string token) {
  for (char& ch : token) {
    if (std::isalnum(static_cast<unsigned char>(ch)) == 0) {
      ch = '_';
    } else {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
  }

  std::string normalized;
  normalized.reserve(token.size());

  bool previousUnderscore = false;
  for (const char ch : token) {
    if (ch == '_') {
      if (!previousUnderscore) {
        normalized.push_back(ch);
      }
      previousUnderscore = true;
      continue;
    }

    normalized.push_back(ch);
    previousUnderscore = false;
  }

  while (!normalized.empty() && normalized.front() == '_') {
    normalized.erase(normalized.begin());
  }
  while (!normalized.empty() && normalized.back() == '_') {
    normalized.pop_back();
  }

  if (normalized.empty()) {
    return "task";
  }
  return normalized;
}

std::string fallbackTarget(const MicroPlanInput& input) {
  if (!input.planId.empty()) {
    return input.planId;
  }
  if (!input.strategyId.empty()) {
    return input.strategyId;
  }
  if (!input.intentId.empty()) {
    return input.intentId;
  }
  return "workspace_root";
}

std::string payloadTargetHint(const TaskPayload& payload) {
  if (payload.kind == TaskPayloadKind::Action) {
    if (!payload.action.target.empty()) {
      return payload.action.target;
    }
    if (!payload.action.id.empty()) {
      return payload.action.id;
    }
    if (payload.action.intentRequest.has_value() &&
        !payload.action.intentRequest->goal.target.empty()) {
      return payload.action.intentRequest->goal.target;
    }
    return {};
  }

  if (!payload.intent.goal.target.empty()) {
    return payload.intent.goal.target;
  }

  return {};
}

bool actionRequiresTarget(const ::ultra::runtime::ActionType type) {
  switch (type) {
    case ::ultra::runtime::ActionType::ImpactPrediction:
    case ::ultra::runtime::ActionType::ContextExtraction:
    case ::ultra::runtime::ActionType::SimulateChange:
      return true;
    default:
      return false;
  }
}

TaskPayload fallbackPayload(const std::string& target) {
  TaskPayload payload;
  payload.kind = TaskPayloadKind::Action;
  payload.action.type = ::ultra::runtime::ActionType::ContextExtraction;
  payload.action.target = target;
  return payload;
}

bool hasMemoryConstraint(const ::ultra::runtime::intent::IntentMemoryContext& memory,
                         const std::string_view value) {
  return std::any_of(memory.knownConstraints.begin(),
                     memory.knownConstraints.end(),
                     [value](const std::string& entry) {
                       return entry == value;
                     });
}

bool memoryContainsHint(const ::ultra::runtime::intent::IntentMemoryContext& memory,
                        const std::initializer_list<std::string_view> hints) {
  const auto containsAny = [&hints](const std::vector<std::string>& values) {
    return std::any_of(values.begin(), values.end(),
                       [&hints](const std::string& entry) {
                         const std::string normalized = normalizeToken(entry);
                         return std::any_of(hints.begin(), hints.end(),
                                            [&normalized](const std::string_view hint) {
                                              return normalized.find(normalizeToken(std::string(hint))) !=
                                                     std::string::npos;
                                            });
                       });
  };

  return containsAny(memory.successfulPatterns) ||
         containsAny(memory.failedPatterns) ||
         containsAny(memory.recoveryPatterns);
}

bool isActionPayloadType(const TaskPayload& payload,
                         const ::ultra::runtime::ActionType type) {
  return payload.kind == TaskPayloadKind::Action && payload.action.type == type;
}

std::size_t firstActionPayloadIndex(const std::vector<TaskPayload>& payloads) {
  for (std::size_t index = 0U; index < payloads.size(); ++index) {
    if (payloads[index].kind == TaskPayloadKind::Action) {
      return index;
    }
  }
  return payloads.size();
}

TaskPayload preflightPayload(const ::ultra::runtime::ActionType type,
                             const std::string& target) {
  TaskPayload payload;
  payload.kind = TaskPayloadKind::Action;
  payload.action.type = type;
  payload.action.target = target;
  return payload;
}

TaskPayload memoryPreflightPayload(
    const std::string& target,
    const ::ultra::runtime::intent::IntentMemoryContext& memory) {
  return preflightPayload(
      memoryContainsHint(memory, {"impact", "dependency", "radius"})
          ? ::ultra::runtime::ActionType::ImpactPrediction
          : ::ultra::runtime::ActionType::ContextExtraction,
      target);
}

void injectAdaptivePreflight(std::vector<TaskPayload>& payloads,
                             const MicroPlanInput& input,
                             const std::string& target) {
  const bool requirePreflight = input.memory.repeatedFailureDetected ||
                                hasMemoryConstraint(input.memory, "require_preflight");
  const bool forceGranularity =
      input.memory.strategyFeedback.increaseTaskGranularity ||
      input.memory.strategyFeedback.forceVariation ||
      input.memory.strategyFeedback.avoidRepeatedPlan;
  if (!(requirePreflight || forceGranularity)) {
    return;
  }

  std::size_t insertionIndex = firstActionPayloadIndex(payloads);
  if (insertionIndex >= payloads.size()) {
    return;
  }

  if (requirePreflight &&
      !(isActionPayloadType(payloads[insertionIndex],
                            ::ultra::runtime::ActionType::ContextExtraction) ||
        isActionPayloadType(payloads[insertionIndex],
                            ::ultra::runtime::ActionType::ImpactPrediction))) {
    payloads.insert(payloads.begin() + insertionIndex,
                    memoryPreflightPayload(target, input.memory));
    ++insertionIndex;
  }

  if (!forceGranularity) {
    return;
  }

  insertionIndex = firstActionPayloadIndex(payloads);
  if (!isActionPayloadType(payloads[insertionIndex],
                           ::ultra::runtime::ActionType::ContextExtraction)) {
    payloads.insert(payloads.begin() + insertionIndex,
                    preflightPayload(::ultra::runtime::ActionType::ContextExtraction,
                                     target));
    ++insertionIndex;
  } else {
    ++insertionIndex;
  }

  if (insertionIndex >= payloads.size() ||
      !isActionPayloadType(payloads[insertionIndex],
                           ::ultra::runtime::ActionType::ImpactPrediction)) {
    payloads.insert(payloads.begin() + std::min(insertionIndex, payloads.size()),
                    preflightPayload(::ultra::runtime::ActionType::ImpactPrediction,
                                     target));
  }
}

TaskPayload normalizePayload(TaskPayload payload,
                             const std::string& target,
                             const std::string& taskId) {
  if (payload.kind == TaskPayloadKind::Action) {
    ::ultra::runtime::Action& action = payload.action;
    const auto logContextFallback = [&](const std::string_view reason) {
      std::cerr << "[MicroPlanner] Invalid payload -> fallback to ContextExtraction."
                << " task=" << taskId
                << " reason=" << reason << "\n";
    };

    if (action.id.empty()) {
      action.id = taskId;
    }

    if (action.type == ::ultra::runtime::ActionType::Mutation &&
        !action.mutation) {
      logContextFallback("mutation action missing mutation callback");
      action.type = ::ultra::runtime::ActionType::ContextExtraction;
      action.target = target;
    }

    if (action.type == ::ultra::runtime::ActionType::BranchDiff &&
        !action.comparisonSnapshot.has_value()) {
      logContextFallback("branch diff action missing comparison snapshot");
      action.type = ::ultra::runtime::ActionType::ContextExtraction;
      action.target = target;
    }

    if (action.type == ::ultra::runtime::ActionType::IntentEvaluation &&
        !action.intentRequest.has_value()) {
      logContextFallback("intent evaluation action missing intent payload");
      action.type = ::ultra::runtime::ActionType::ContextExtraction;
      action.target = target;
    }

    if (action.type == ::ultra::runtime::ActionType::ModelGenerate &&
        (!action.modelRequest.has_value() ||
         action.modelRequest->prompt.empty() ||
         (!action.orchestrationContext.has_value() &&
          action.modelProvider.empty()))) {
      logContextFallback("model generate action missing prompt/provider context");
      action.type = ::ultra::runtime::ActionType::ContextExtraction;
      action.target = target;
    }

    if (actionRequiresTarget(action.type) && action.target.empty()) {
      action.target = target;
    }

    return payload;
  }

  if (payload.intent.goal.target.empty()) {
    payload.intent.goal.target = target;
  }

  return payload;
}

std::string makeTaskId(const std::size_t index,
                       const TaskPayload& payload,
                       const std::string& target) {
  const std::string kindPrefix =
      payload.kind == TaskPayloadKind::Action ? "action" : "intent";

  const std::string rawHint = payloadTargetHint(payload).empty()
                                  ? target
                                  : payloadTargetHint(payload);

  char ordinal[16] = {};
  std::snprintf(ordinal, sizeof(ordinal), "%04zu", index + 1U);

  return kindPrefix + "_" + std::string(ordinal) + "_" +
         normalizeToken(rawHint);
}

}  // namespace

TaskGraph MicroPlanner::generate_plan(const MicroPlanInput& input) const {
  const std::string target = fallbackTarget(input);

  std::vector<TaskPayload> payloads = input.taskPayloads;
  if (payloads.empty()) {
    payloads.push_back(fallbackPayload(target));
  }
  injectAdaptivePreflight(payloads, input, target);

  TaskGraph graph;
  std::string previousTaskId;

  for (std::size_t index = 0U; index < payloads.size(); ++index) {
    const std::string taskId = makeTaskId(index, payloads[index], target);

    TaskNode node;
    node.id = taskId;
    node.payload = normalizePayload(std::move(payloads[index]), target, taskId);

    if (!previousTaskId.empty()) {
      node.dependencies.push_back(previousTaskId);
    }

    if (!graph.add_task(std::move(node))) {
      return TaskGraph{};
    }

    previousTaskId = taskId;
  }

  return graph;
}

}  // namespace ultra::runtime::cognitive
