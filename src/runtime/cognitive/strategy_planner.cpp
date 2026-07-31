#include "strategy_planner.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ultra::runtime::cognitive {

namespace {

std::string trimCopy(const std::string_view value) {
  std::size_t start = 0U;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }

  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }

  return std::string(value.substr(start, end - start));
}

void logStrategyParserFailure(const std::string_view reason,
                              const model::ModelResponse& response) {
  std::cerr << "[StrategyParser] Failed to parse model output: " << reason
            << " strategy_len=" << response.strategyText.size();
  if (!response.errorMessage.empty()) {
    std::cerr << " error=" << response.errorMessage;
  }
  std::cerr << "\n";
}

std::string normalizeToken(const std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());

  bool previousUnderscore = false;
  for (char ch : value) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0) {
      normalized.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
      previousUnderscore = false;
      continue;
    }

    if (!previousUnderscore && !normalized.empty()) {
      normalized.push_back('_');
    }
    previousUnderscore = true;
  }

  while (!normalized.empty() && normalized.back() == '_') {
    normalized.pop_back();
  }

  if (normalized.empty()) {
    return "value";
  }

  return normalized;
}

std::string defaultTarget(const intent::Intent& intentValue) {
  if (!intentValue.goal.target.empty()) {
    return intentValue.goal.target;
  }
  if (!intentValue.constraints.branchScope.empty()) {
    return intentValue.constraints.branchScope;
  }
  return "workspace_root";
}

intent::ActionKind actionKindForGoal(const intent::GoalType goalType) {
  switch (goalType) {
    case intent::GoalType::ModifySymbol:
      return intent::ActionKind::ModifySymbolBody;
    case intent::GoalType::RefactorModule:
      return intent::ActionKind::RefactorModule;
    case intent::GoalType::ReduceImpactRadius:
      return intent::ActionKind::ReduceImpactRadius;
    case intent::GoalType::ImproveCentrality:
      return intent::ActionKind::ImproveCentrality;
    case intent::GoalType::MinimizeTokenUsage:
      return intent::ActionKind::MinimizeTokenUsage;
    case intent::GoalType::AddDependency:
      return intent::ActionKind::AddDependency;
    case intent::GoalType::RemoveDependency:
      return intent::ActionKind::RemoveDependency;
  }
  return intent::ActionKind::ModifySymbolBody;
}

double riskValueForTolerance(const intent::RiskTolerance tolerance) {
  switch (tolerance) {
    case intent::RiskTolerance::LOW:
      return 0.25;
    case intent::RiskTolerance::MEDIUM:
      return 0.50;
    case intent::RiskTolerance::HIGH:
      return 0.75;
  }
  return 0.50;
}

bool hasMemoryConstraint(const intent::IntentMemoryContext& memory,
                         const std::string_view value) {
  return std::any_of(memory.knownConstraints.begin(),
                     memory.knownConstraints.end(),
                     [value](const std::string& entry) {
                       return entry == value;
                     });
}

std::string firstMemoryPattern(const std::vector<std::string>& values) {
  return values.empty() ? std::string{} : values.front();
}

double aggregateFeedbackScore(const intent::StrategyScore& score) {
  return std::clamp((score.successRate * 0.35) +
                        ((1.0 - score.recoveryCost) * 0.20) +
                        (score.executionEfficiency * 0.25) +
                        (score.confidenceScore * 0.20),
                    0.0,
                    1.0);
}

nlohmann::ordered_json recentPlanPerformanceJson(
    const intent::IntentMemoryContext& memory) {
  nlohmann::ordered_json recentPlans = nlohmann::ordered_json::array();
  for (const intent::PlanPerformance& plan : memory.recentPlanPerformance) {
    recentPlans.push_back({
        {"plan_hash", plan.planHash},
        {"score", plan.score},
        {"iteration_index", plan.iterationIndex},
        {"strategy_type", plan.strategyType},
    });
  }
  return recentPlans;
}

intent::ActionKind memoryAdjustedActionKind(const intent::Intent& intentValue) {
  const bool guarded = intentValue.memory.repeatedFailureDetected ||
                       hasMemoryConstraint(intentValue.memory, "tight_scope") ||
                       intentValue.memory.strategyFeedback.simplifyPlan ||
                       intentValue.memory.strategyFeedback.increaseTaskGranularity ||
                       intentValue.memory.strategyFeedback.forceVariation;
  if (!guarded) {
    return actionKindForGoal(intentValue.goal.type);
  }

  if (intentValue.goal.type == intent::GoalType::RefactorModule) {
    return intent::ActionKind::ModifySymbolBody;
  }

  return actionKindForGoal(intentValue.goal.type);
}

std::string memoryStrategyName(const intent::Intent& intentValue) {
  const intent::StrategyFeedbackMemory& feedback = intentValue.memory.strategyFeedback;
  if (feedback.reinforcePattern && !feedback.preferredStrategyType.empty()) {
    return "feedback_reuse_" + normalizeToken(feedback.preferredStrategyType);
  }
  if (feedback.forceVariation || feedback.avoidRepeatedPlan) {
    return "feedback_varied_" + normalizeToken(intent::toString(intentValue.goal.type));
  }
  if (feedback.simplifyPlan || feedback.increaseTaskGranularity ||
      intentValue.memory.repeatedFailureDetected) {
    return "feedback_guarded_" + normalizeToken(intent::toString(intentValue.goal.type));
  }
  if (intentValue.memory.hasReusableStrategy) {
    return "memory_reuse_" + normalizeToken(intent::toString(intentValue.goal.type));
  }
  return "deterministic_" + normalizeToken(intent::toString(intentValue.goal.type));
}

std::string memoryStrategyDetails(const intent::Intent& intentValue) {
  std::string details = "Deterministic fallback strategy action.";
  const intent::StrategyFeedbackMemory& feedback = intentValue.memory.strategyFeedback;

  if (feedback.reinforcePattern && !feedback.preferredStrategyType.empty()) {
    details = "Reinforce high-score strategy: " + feedback.preferredStrategyType;
  } else if (intentValue.memory.hasReusableStrategy) {
    const std::string pattern = firstMemoryPattern(intentValue.memory.successfulPatterns);
    if (!pattern.empty()) {
      details = "Reuse successful pattern: " + pattern;
    }
  }

  const std::string failedPattern = firstMemoryPattern(intentValue.memory.failedPatterns);
  if (!failedPattern.empty()) {
    details += " Avoid prior failure: " + failedPattern;
  }

  if (!feedback.strategyType.empty()) {
    details += " Last strategy=" + feedback.strategyType;
    details += " score=" + std::to_string(aggregateFeedbackScore(feedback.latestScore));
  }
  if (feedback.forceVariation) {
    details += " Force variation from repeated low-score plan.";
  }
  if (feedback.simplifyPlan) {
    details += " Simplify execution after recovery-heavy iteration.";
  }
  if (feedback.increaseTaskGranularity) {
    details += " Increase task granularity.";
  }
  if (!feedback.avoidedStrategyType.empty()) {
    details += " Avoid strategy: " + feedback.avoidedStrategyType;
  }

  return details;
}

std::size_t memoryScopedFilesChanged(const intent::Intent& intentValue) {
  const std::size_t filesConstraint =
      std::max<std::size_t>(1U, intentValue.constraints.maxFilesChanged);
  const intent::StrategyFeedbackMemory& feedback = intentValue.memory.strategyFeedback;
  if (intentValue.memory.repeatedFailureDetected ||
      hasMemoryConstraint(intentValue.memory, "tight_scope") ||
      feedback.simplifyPlan || feedback.increaseTaskGranularity ||
      feedback.forceVariation) {
    return 1U;
  }
  if (feedback.reinforcePattern || intentValue.memory.hasReusableStrategy) {
    return std::min<std::size_t>(filesConstraint, 2U);
  }
  return 1U;
}

std::size_t memoryScopedDependencyDepth(const intent::Intent& intentValue) {
  const std::size_t depthConstraint =
      std::max<std::size_t>(1U, intentValue.constraints.maxImpactDepth);
  const intent::StrategyFeedbackMemory& feedback = intentValue.memory.strategyFeedback;
  if (intentValue.memory.repeatedFailureDetected ||
      hasMemoryConstraint(intentValue.memory, "tight_scope") ||
      feedback.simplifyPlan || feedback.increaseTaskGranularity ||
      feedback.forceVariation) {
    return 1U;
  }
  if (feedback.reinforcePattern || intentValue.memory.hasReusableStrategy) {
    return std::min<std::size_t>(depthConstraint, 2U);
  }
  return 1U;
}

double memoryAdjustedRisk(const intent::Intent& intentValue) {
  const intent::StrategyFeedbackMemory& feedback = intentValue.memory.strategyFeedback;
  double value = riskValueForTolerance(intentValue.risk);
  if (feedback.reinforcePattern && !feedback.forceVariation &&
      !intentValue.memory.repeatedFailureDetected) {
    value = std::max(0.10, value - 0.15);
  } else if (intentValue.memory.hasReusableStrategy &&
             !intentValue.memory.repeatedFailureDetected) {
    value = std::max(0.10, value - 0.10);
  }
  if (intentValue.memory.repeatedFailureDetected || feedback.simplifyPlan ||
      feedback.increaseTaskGranularity || feedback.forceVariation) {
    value = std::min(value, 0.30);
  }
  if (feedback.reinforcePattern && feedback.latestScore.confidenceScore >= 0.80) {
    value = std::max(0.10, value - 0.05);
  }
  return value;
}

std::optional<std::size_t> parseSizeToken(const std::string_view token) {
  const std::string trimmed = trimCopy(token);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  std::size_t value = 0U;
  const char* first = trimmed.data();
  const char* last = trimmed.data() + trimmed.size();
  const std::from_chars_result result = std::from_chars(first, last, value);
  if (result.ec != std::errc{} || result.ptr != last) {
    return std::nullopt;
  }

  return value;
}

std::optional<double> parseDoubleToken(const std::string_view token) {
  const std::string trimmed = trimCopy(token);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  char* end = nullptr;
  const double value = std::strtod(trimmed.c_str(), &end);
  if (end == nullptr || *end != '\0') {
    return std::nullopt;
  }

  return value;
}

std::optional<bool> parseBoolToken(const std::string_view token) {
  const std::string normalized = normalizeToken(token);
  if (normalized == "true" || normalized == "yes" || normalized == "1") {
    return true;
  }
  if (normalized == "false" || normalized == "no" || normalized == "0") {
    return false;
  }
  return std::nullopt;
}

std::map<std::string, std::string> parseTextFields(const std::string& text) {
  std::map<std::string, std::string> fields;

  std::size_t offset = 0U;
  while (offset < text.size()) {
    const std::size_t end = text.find('\n', offset);
    const std::string_view line =
        end == std::string::npos
            ? std::string_view(text).substr(offset)
            : std::string_view(text).substr(offset, end - offset);

    const std::size_t separator = line.find(':');
    if (separator != std::string_view::npos) {
      std::string key = normalizeToken(line.substr(0U, separator));
      std::string value = trimCopy(line.substr(separator + 1U));
      if (!key.empty() && !value.empty()) {
        fields.try_emplace(std::move(key), std::move(value));
      }
    }

    if (end == std::string::npos) {
      break;
    }
    offset = end + 1U;
  }

  return fields;
}

std::optional<std::string> lookupJsonString(
    const nlohmann::ordered_json& value,
    const std::initializer_list<const char*> keys) {
  if (!value.is_object()) {
    return std::nullopt;
  }

  for (const char* key : keys) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_string()) {
      continue;
    }

    const std::string parsed = trimCopy(it->get_ref<const std::string&>());
    if (!parsed.empty()) {
      return parsed;
    }
  }

  return std::nullopt;
}

std::optional<std::size_t> lookupJsonSize(
    const nlohmann::ordered_json& value,
    const std::initializer_list<const char*> keys) {
  if (!value.is_object()) {
    return std::nullopt;
  }

  for (const char* key : keys) {
    const auto it = value.find(key);
    if (it == value.end()) {
      continue;
    }

    if (it->is_number_unsigned()) {
      return it->get<std::size_t>();
    }
    if (it->is_number_integer()) {
      const auto raw = it->get<long long>();
      if (raw >= 0LL) {
        return static_cast<std::size_t>(raw);
      }
      continue;
    }
    if (it->is_string()) {
      const auto parsed = parseSizeToken(it->get_ref<const std::string&>());
      if (parsed.has_value()) {
        return parsed;
      }
    }
  }

  return std::nullopt;
}

std::optional<double> lookupJsonDouble(
    const nlohmann::ordered_json& value,
    const std::initializer_list<const char*> keys) {
  if (!value.is_object()) {
    return std::nullopt;
  }

  for (const char* key : keys) {
    const auto it = value.find(key);
    if (it == value.end()) {
      continue;
    }

    if (it->is_number_float() || it->is_number_integer() ||
        it->is_number_unsigned()) {
      return it->get<double>();
    }

    if (it->is_string()) {
      const auto parsed = parseDoubleToken(it->get_ref<const std::string&>());
      if (parsed.has_value()) {
        return parsed;
      }
    }
  }

  return std::nullopt;
}

std::optional<bool> lookupJsonBool(
    const nlohmann::ordered_json& value,
    const std::initializer_list<const char*> keys) {
  if (!value.is_object()) {
    return std::nullopt;
  }

  for (const char* key : keys) {
    const auto it = value.find(key);
    if (it == value.end()) {
      continue;
    }

    if (it->is_boolean()) {
      return it->get<bool>();
    }

    if (it->is_string()) {
      const auto parsed = parseBoolToken(it->get_ref<const std::string&>());
      if (parsed.has_value()) {
        return parsed;
      }
    }
  }

  return std::nullopt;
}

std::optional<std::string> lookupTextValue(
    const std::map<std::string, std::string>& fields,
    const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto it = fields.find(normalizeToken(key));
    if (it != fields.end() && !it->second.empty()) {
      return it->second;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> lookupTextSize(
    const std::map<std::string, std::string>& fields,
    const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto it = fields.find(normalizeToken(key));
    if (it == fields.end()) {
      continue;
    }
    const auto parsed = parseSizeToken(it->second);
    if (parsed.has_value()) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<double> lookupTextDouble(
    const std::map<std::string, std::string>& fields,
    const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto it = fields.find(normalizeToken(key));
    if (it == fields.end()) {
      continue;
    }
    const auto parsed = parseDoubleToken(it->second);
    if (parsed.has_value()) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<bool> lookupTextBool(
    const std::map<std::string, std::string>& fields,
    const std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto it = fields.find(normalizeToken(key));
    if (it == fields.end()) {
      continue;
    }
    const auto parsed = parseBoolToken(it->second);
    if (parsed.has_value()) {
      return parsed;
    }
  }
  return std::nullopt;
}

std::optional<intent::ActionKind> parseActionKindToken(
    const std::string_view rawToken) {
  const std::string normalized = normalizeToken(rawToken);
  if (normalized == "modify_symbol_body" || normalized == "modifysymbolbody") {
    return intent::ActionKind::ModifySymbolBody;
  }
  if (normalized == "refactor_module" || normalized == "refactormodule") {
    return intent::ActionKind::RefactorModule;
  }
  if (normalized == "reduce_impact_radius" ||
      normalized == "reduceimpactradius") {
    return intent::ActionKind::ReduceImpactRadius;
  }
  if (normalized == "improve_centrality" || normalized == "improvecentrality") {
    return intent::ActionKind::ImproveCentrality;
  }
  if (normalized == "minimize_token_usage" ||
      normalized == "minimizetokenusage") {
    return intent::ActionKind::MinimizeTokenUsage;
  }
  if (normalized == "add_dependency" || normalized == "adddependency") {
    return intent::ActionKind::AddDependency;
  }
  if (normalized == "remove_dependency" || normalized == "removedependency") {
    return intent::ActionKind::RemoveDependency;
  }
  if (normalized == "rename_symbol" || normalized == "renamesymbol") {
    return intent::ActionKind::RenameSymbol;
  }
  if (normalized == "change_signature" || normalized == "changesignature") {
    return intent::ActionKind::ChangeSignature;
  }
  if (normalized == "update_public_api" || normalized == "updatepublicapi") {
    return intent::ActionKind::UpdatePublicAPI;
  }
  if (normalized == "move_across_modules" ||
      normalized == "moveacrossmodules") {
    return intent::ActionKind::MoveAcrossModules;
  }
  return std::nullopt;
}

model::ModelRequest buildModelRequest(const intent::Intent& intentValue) {
  model::ModelRequest request;
  request.intentReference =
      intent::toString(intentValue.goal.type) + ":" + intentValue.goal.target;

  const std::string baseInstruction =
      "Generate one strategy step for UltraInfinity. Return either structured "
      "JSON fields or key-value lines with keys: name, action_kind, target, "
      "details, estimated_files_changed, estimated_dependency_depth, "
      "public_api_surface.";

  std::string intentGuidance;
  if (!intentValue.rawPrompt.empty()) {
    const std::string lp = intentValue.rawPrompt;
    if (lp.find("fix") != std::string::npos || lp.find("bug") != std::string::npos || lp.find("error") != std::string::npos || lp.find("crash") != std::string::npos) {
      intentGuidance = "Strategy type: bugfix. Flow: gather diagnostics, read active file, check build errors, generate targeted patch, verify fix, retry if same error.";
    } else if (lp.find("explain") != std::string::npos || lp.find("why") != std::string::npos) {
      intentGuidance = "Strategy type: analysis. Flow: gather source and symbol graph, produce explanation only. Do NOT generate patches.";
    } else if (lp.find("test") != std::string::npos || lp.find("assert") != std::string::npos) {
      intentGuidance = "Strategy type: testing. Flow: discover test framework, generate test files, run tests if possible.";
    } else if (lp.find("refactor") != std::string::npos || lp.find("clean") != std::string::npos) {
      intentGuidance = "Strategy type: refactor. Flow: analyze dependencies, apply safe structural edits, preserve behavior, run tests if available.";
    } else if (lp.find("optimize") != std::string::npos || lp.find("performance") != std::string::npos || lp.find("faster") != std::string::npos) {
      intentGuidance = "Strategy type: optimization. Flow: identify hot paths, propose improvements, benchmark if possible.";
    } else if (lp.find("architecture") != std::string::npos || lp.find("design") != std::string::npos) {
      intentGuidance = "Strategy type: architecture. Flow: analyze module graph, produce architecture explanation. Do NOT generate patches.";
    } else {
      intentGuidance = "Strategy type: implementation. Flow: plan files to create or modify, generate code, patch files, verify.";
    }
    std::cerr << "[ULTRA-PLANNER] " << intentGuidance << std::endl;
    request.prompt = "User Intent: " + intentValue.rawPrompt + "\n" + intentGuidance + "\n\n" + baseInstruction;
  } else {
    request.prompt = baseInstruction;
  }

  request.context = {
      {"goal", { {"type", intent::toString(intentValue.goal.type)},
                  {"target", intentValue.goal.target} }},
      {"risk_tolerance", intent::toString(intentValue.risk)},
      {"branch_scope", intentValue.constraints.branchScope},
      {"raw_prompt", intentValue.rawPrompt},
      {"memory", {
          {"query_key", intentValue.memory.queryKey},
          {"successful_patterns", intentValue.memory.successfulPatterns},
          {"failed_patterns", intentValue.memory.failedPatterns},
          {"known_constraints", intentValue.memory.knownConstraints},
          {"prior_outcomes", intentValue.memory.priorOutcomes},
          {"repeated_failure_detected", intentValue.memory.repeatedFailureDetected},
          {"strategy_feedback", {
              {"strategy_type", intentValue.memory.strategyFeedback.strategyType},
              {"outcome", intentValue.memory.strategyFeedback.outcome},
              {"plan_hash", intentValue.memory.strategyFeedback.planHash},
              {"reinforce_pattern", intentValue.memory.strategyFeedback.reinforcePattern},
              {"simplify_plan", intentValue.memory.strategyFeedback.simplifyPlan},
              {"increase_task_granularity", intentValue.memory.strategyFeedback.increaseTaskGranularity},
              {"avoid_repeated_plan", intentValue.memory.strategyFeedback.avoidRepeatedPlan},
              {"force_variation", intentValue.memory.strategyFeedback.forceVariation},
              {"preferred_strategy_type", intentValue.memory.strategyFeedback.preferredStrategyType},
              {"avoided_strategy_type", intentValue.memory.strategyFeedback.avoidedStrategyType},
              {"latest_score", {
                  {"success_rate", intentValue.memory.strategyFeedback.latestScore.successRate},
                  {"failure_count", intentValue.memory.strategyFeedback.latestScore.failureCount},
                  {"recovery_cost", intentValue.memory.strategyFeedback.latestScore.recoveryCost},
                  {"execution_efficiency", intentValue.memory.strategyFeedback.latestScore.executionEfficiency},
                  {"confidence_score", intentValue.memory.strategyFeedback.latestScore.confidenceScore},
                  {"overall_score", aggregateFeedbackScore(intentValue.memory.strategyFeedback.latestScore)}
              }},
              {"recent_plans", recentPlanPerformanceJson(intentValue.memory)}
          }}
      }},
  };

  request.constraints = {
      {"max_impact_depth", intentValue.constraints.maxImpactDepth},
      {"max_files_changed", intentValue.constraints.maxFilesChanged},
      {"token_budget", intentValue.constraints.tokenBudget},
      {"determinism_required", intentValue.constraints.determinismRequired},
      {"allow_public_api_change", intentValue.options.allowPublicAPIChange},
  };

  return request;
}

std::optional<intent::Strategy> parseModelStrategy(
    const model::ModelResponse& response,
    const intent::Intent& intentValue) {
  if (!response.ok) {
    logStrategyParserFailure("model response not ok", response);
    return std::nullopt;
  }

  const auto textFields = parseTextFields(response.strategyText);

  const std::string name = lookupJsonString(response.structuredOutput,
                                            {"name", "strategy_name"})
                               .value_or(lookupTextValue(textFields,
                                                         {"name", "strategy_name"})
                                             .value_or(std::string{}));

  const std::string actionKindToken =
      lookupJsonString(response.structuredOutput,
                       {"action_kind", "kind", "action"})
          .value_or(lookupTextValue(textFields,
                                    {"action_kind", "kind", "action"})
                        .value_or(std::string{}));

  const std::optional<intent::ActionKind> actionKind =
      parseActionKindToken(actionKindToken);
  if (!actionKind.has_value()) {
    logStrategyParserFailure("missing or invalid action kind", response);
    return std::nullopt;
  }

  const std::string target =
      lookupJsonString(response.structuredOutput, {"target"})
          .value_or(lookupTextValue(textFields, {"target"})
                        .value_or(defaultTarget(intentValue)));

  if (target.empty()) {
    logStrategyParserFailure("missing target", response);
    return std::nullopt;
  }

  const std::string details =
      lookupJsonString(response.structuredOutput, {"details", "description"})
          .value_or(lookupTextValue(textFields, {"details", "description"})
                        .value_or("Model-assisted strategy action."));

  const std::size_t filesConstraint =
      std::max<std::size_t>(1U, intentValue.constraints.maxFilesChanged);
  const std::size_t depthConstraint =
      std::max<std::size_t>(1U, intentValue.constraints.maxImpactDepth);

  const std::size_t estimatedFilesChanged = std::clamp<std::size_t>(
      lookupJsonSize(response.structuredOutput, {"estimated_files_changed", "files"})
          .value_or(lookupTextSize(textFields,
                                   {"estimated_files_changed", "files"})
                        .value_or(1U)),
      1U,
      filesConstraint);

  const std::size_t estimatedDependencyDepth = std::clamp<std::size_t>(
      lookupJsonSize(response.structuredOutput,
                     {"estimated_dependency_depth", "dependency_depth", "depth"})
          .value_or(lookupTextSize(textFields,
                                   {"estimated_dependency_depth",
                                    "dependency_depth",
                                    "depth"})
                        .value_or(1U)),
      1U,
      depthConstraint);

  const bool publicApiSurface =
      lookupJsonBool(response.structuredOutput,
                     {"public_api_surface", "public_api"})
          .value_or(lookupTextBool(textFields,
                                   {"public_api_surface", "public_api"})
                        .value_or(false));

  const double riskValue = std::clamp(
      lookupJsonDouble(response.structuredOutput, {"risk_value", "risk"})
          .value_or(lookupTextDouble(textFields, {"risk_value", "risk"})
                        .value_or(riskValueForTolerance(intentValue.risk))),
      0.0,
      1.0);

  intent::Strategy strategy;
  strategy.name = name.empty()
                      ? "model_assisted_" +
                            normalizeToken(intent::toString(intentValue.goal.type))
                      : normalizeToken(name);

  intent::Action action;
  action.kind = *actionKind;
  action.target = target;
  action.details = details;
  action.estimatedFilesChanged = estimatedFilesChanged;
  action.estimatedDependencyDepth = estimatedDependencyDepth;
  action.publicApiSurface = publicApiSurface;
  strategy.proposedActions.push_back(std::move(action));

  strategy.risk.value = riskValue;
  strategy.risk.classification = intentValue.risk;
  strategy.risk.tolerance = intentValue.risk;

  strategy.impact.radius = riskValue;
  strategy.impact.estimatedFiles = estimatedFilesChanged;
  strategy.impact.dependencyDepth = estimatedDependencyDepth;
  strategy.impact.centrality = 0.0;
  strategy.impact.maxFilesConstraint = filesConstraint;
  strategy.impact.maxDepthConstraint = depthConstraint;

  strategy.determinism.required = intentValue.constraints.determinismRequired;
  strategy.determinism.value =
      intentValue.constraints.determinismRequired ? 0.90 : 0.80;

  strategy.tokenCost.budget = intentValue.constraints.tokenBudget;
  strategy.tokenCost.estimatedTokens = std::min<std::size_t>(
      strategy.tokenCost.budget,
      std::max<std::size_t>(
          1U,
          (estimatedFilesChanged * 128U) + (estimatedDependencyDepth * 64U)));
  strategy.tokenCost.withinBudget =
      strategy.tokenCost.estimatedTokens <= strategy.tokenCost.budget;

  return strategy;
}

::ultra::runtime::ActionType actionTypeForKind(const intent::ActionKind kind) {
  switch (kind) {
    case intent::ActionKind::ReduceImpactRadius:
    case intent::ActionKind::ImproveCentrality:
      return ::ultra::runtime::ActionType::ImpactPrediction;
    case intent::ActionKind::MinimizeTokenUsage:
      return ::ultra::runtime::ActionType::ContextExtraction;
    case intent::ActionKind::ModifySymbolBody:
    case intent::ActionKind::RefactorModule:
    case intent::ActionKind::AddDependency:
    case intent::ActionKind::RemoveDependency:
    case intent::ActionKind::RenameSymbol:
    case intent::ActionKind::ChangeSignature:
    case intent::ActionKind::UpdatePublicAPI:
    case intent::ActionKind::MoveAcrossModules:
      return ::ultra::runtime::ActionType::SimulateChange;
  }
  return ::ultra::runtime::ActionType::SimulateChange;
}

std::string makeActionId(const std::size_t index,
                         const std::string& strategyName,
                         const std::string& target) {
  char ordinal[16] = {};
  std::snprintf(ordinal, sizeof(ordinal), "%04zu", index + 1U);
  return normalizeToken(strategyName) + "_" + std::string(ordinal) + "_" +
         normalizeToken(target);
}

intent::Intent planningIntentFromFrame(const UltraLoopFrame& frame) {
  if (frame.hasStructuredIntent) {
    return frame.structuredIntent;
  }

  intent::Intent intentValue;
  intentValue.goal.type = intent::GoalType::ModifySymbol;
  intentValue.goal.target =
      frame.intentTarget.empty() ? frame.intentGoal : frame.intentTarget;
  intentValue.constraints.maxImpactDepth = frame.intentImpactDepth;
  intentValue.constraints.maxFilesChanged = frame.intentMaxFilesChanged;
  intentValue.constraints.tokenBudget = frame.intentTokenBudget;
  intentValue.constraints.branchScope = frame.intentBranchId;
  intentValue.constraints.determinismRequired = true;
  intentValue.risk = frame.intentTolerance;
  intentValue.options.allowPublicAPIChange = frame.intentAllowPublicApiChange;

  const std::size_t fallbackTokenBudget =
      frame.intentTokenBudget == 0U ? 4096U : frame.intentTokenBudget;
  return intent::normalizeIntent(intentValue, fallbackTokenBudget);
}

std::vector<TaskPayload> payloadsFromStrategy(const intent::Strategy& strategy,
                                              const intent::Intent& intentValue) {
  std::vector<TaskPayload> payloads;
  payloads.reserve(strategy.proposedActions.size());

  const std::string fallbackTarget = defaultTarget(intentValue);

  for (std::size_t index = 0U; index < strategy.proposedActions.size(); ++index) {
    const intent::Action& strategyAction = strategy.proposedActions[index];

    TaskPayload payload;
    payload.kind = TaskPayloadKind::Action;
    payload.action.type = actionTypeForKind(strategyAction.kind);
    payload.action.target =
        strategyAction.target.empty() ? fallbackTarget : strategyAction.target;
    payload.action.id = makeActionId(index, strategy.name, payload.action.target);
    payload.action.riskScore = strategy.risk.value;
    payload.action.confidenceScore = strategy.determinism.value;
    payload.action.intentRequest = intentValue;
    payload.plannedAction = strategyAction;

    payloads.push_back(std::move(payload));
  }

  return payloads;
}

}  // namespace

intent::Strategy StrategyPlanner::generate(const intent::Intent& intentValue) const {
  const std::size_t fallbackTokenBudget =
      intentValue.constraints.tokenBudget == 0U ? 4096U
                                                : intentValue.constraints.tokenBudget;
  const intent::Intent normalizedIntent =
      intent::normalizeIntent(intentValue, fallbackTokenBudget);

  if (const std::optional<intent::Strategy> modelStrategy =
          tryGenerateWithModel(normalizedIntent);
      modelStrategy.has_value()) {
    return *modelStrategy;
  }

  return generateDeterministic(normalizedIntent);
}

void StrategyPlanner::setModel(model::IModel* model) noexcept {
  model_ = model;
}

intent::Strategy StrategyPlanner::generateDeterministic(
    const intent::Intent& intentValue) const {
  intent::Strategy strategy;
  strategy.name = memoryStrategyName(intentValue);

  const std::size_t estimatedFilesChanged = memoryScopedFilesChanged(intentValue);
  const std::size_t estimatedDependencyDepth =
      memoryScopedDependencyDepth(intentValue);

  intent::Action action;
  action.kind = memoryAdjustedActionKind(intentValue);
  action.target = defaultTarget(intentValue);
  action.details = memoryStrategyDetails(intentValue);
  action.estimatedFilesChanged = estimatedFilesChanged;
  action.estimatedDependencyDepth = estimatedDependencyDepth;
  action.publicApiSurface = intentValue.options.allowPublicAPIChange &&
                            !hasMemoryConstraint(intentValue.memory,
                                                 "avoid_public_api");
  strategy.proposedActions.push_back(std::move(action));

  strategy.risk.value = memoryAdjustedRisk(intentValue);
  strategy.risk.classification = intentValue.risk;
  strategy.risk.tolerance = intentValue.risk;

  strategy.impact.radius = strategy.risk.value;
  strategy.impact.estimatedFiles = estimatedFilesChanged;
  strategy.impact.dependencyDepth = estimatedDependencyDepth;
  strategy.impact.centrality = 0.0;
  strategy.impact.maxFilesConstraint =
      std::max<std::size_t>(1U, intentValue.constraints.maxFilesChanged);
  strategy.impact.maxDepthConstraint =
      std::max<std::size_t>(1U, intentValue.constraints.maxImpactDepth);

  strategy.determinism.required = intentValue.constraints.determinismRequired;
  strategy.determinism.value =
      intentValue.constraints.determinismRequired ? 1.0 : 0.95;

  strategy.tokenCost.budget = intentValue.constraints.tokenBudget;
  strategy.tokenCost.estimatedTokens = std::min<std::size_t>(
      strategy.tokenCost.budget,
      std::max<std::size_t>(
          1U,
          (estimatedFilesChanged * 128U) + (estimatedDependencyDepth * 64U)));
  strategy.tokenCost.withinBudget =
      strategy.tokenCost.estimatedTokens <= strategy.tokenCost.budget;

  return strategy;
}

std::optional<intent::Strategy> StrategyPlanner::tryGenerateWithModel(
    const intent::Intent& intentValue) const {
  if (model_ == nullptr) {
    return std::nullopt;
  }

  try {
    const model::ModelRequest request = buildModelRequest(intentValue);
    const model::ModelResponse response = model_->generate(request);
    if (std::optional<intent::Strategy> strategy =
            parseModelStrategy(response, intentValue);
        strategy.has_value()) {
      return strategy;
    }

    intent::Strategy fallback = generateDeterministic(intentValue);
    fallback.name = response.ok ? "fallback_due_to_parse_failure"
                                : "fallback_due_to_model_failure";
    return fallback;
  } catch (...) {
    std::cerr << "[StrategyParser] Model-assisted strategy generation threw; using deterministic fallback.\n";
    intent::Strategy fallback = generateDeterministic(intentValue);
    fallback.name = "fallback_due_to_model_exception";
    return fallback;
  }
}

StrategyPlanningStage::StrategyPlanningStage(StrategyPlanner planner)
    : planner_(std::move(planner)) {}

void StrategyPlanningStage::setModel(model::IModel* model) noexcept {
  planner_.setModel(model);
}

StageResult StrategyPlanningStage::run(UltraLoopFrame& frame) {
  StageResult result;

  const intent::Intent planningIntent = planningIntentFromFrame(frame);
  std::cerr << "[ULTRA-PLANNER] Strategy=" << intent::toString(planningIntent.goal.type)
            << " target=" << planningIntent.goal.target
            << " rawPrompt=" << (planningIntent.rawPrompt.empty() ? "<none>" : planningIntent.rawPrompt.substr(0, 80))
            << std::endl;
  const intent::Strategy strategy = planner_.generate(planningIntent);

  frame.strategyId = strategy.name.empty()
                         ? "strategy_" + std::to_string(frame.iteration)
                         : strategy.name;
  frame.microTaskPayloads = payloadsFromStrategy(strategy, planningIntent);

  std::cerr << "[ULTRA-PLANNER] Generated strategy=" << frame.strategyId
            << " actions=" << frame.microTaskPayloads.size() << std::endl;

  if (frame.microTaskPayloads.empty()) {
    result.success = false;
    result.signal = StageSignal::Replan;
    result.message = "Strategy planner produced no executable actions.";
    return result;
  }

  result.success = true;
  result.signal = StageSignal::Continue;
  result.message = "Strategy planner produced deterministic execution actions.";
  return result;
}

}  // namespace ultra::runtime::cognitive

