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

  request.prompt =
      "Generate one strategy step for UltraInfinity. Return either structured "
      "JSON fields or key-value lines with keys: name, action_kind, target, "
      "details, estimated_files_changed, estimated_dependency_depth, "
      "public_api_surface.";

  request.context = {
      {"goal", { {"type", intent::toString(intentValue.goal.type)},
                  {"target", intentValue.goal.target} }},
      {"risk_tolerance", intent::toString(intentValue.risk)},
      {"branch_scope", intentValue.constraints.branchScope},
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
  strategy.name =
      "deterministic_" + normalizeToken(intent::toString(intentValue.goal.type));

  intent::Action action;
  action.kind = actionKindForGoal(intentValue.goal.type);
  action.target = defaultTarget(intentValue);
  action.details = "Deterministic fallback strategy action.";
  action.estimatedFilesChanged = 1U;
  action.estimatedDependencyDepth = 1U;
  action.publicApiSurface = false;
  strategy.proposedActions.push_back(std::move(action));

  strategy.risk.value = riskValueForTolerance(intentValue.risk);
  strategy.risk.classification = intentValue.risk;
  strategy.risk.tolerance = intentValue.risk;

  strategy.impact.radius = strategy.risk.value;
  strategy.impact.estimatedFiles = 1U;
  strategy.impact.dependencyDepth = 1U;
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
      std::max<std::size_t>(1U, strategy.tokenCost.budget / 8U));
  strategy.tokenCost.withinBudget = true;

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
  const intent::Strategy strategy = planner_.generate(planningIntent);

  frame.strategyId = strategy.name.empty()
                         ? "strategy_" + std::to_string(frame.iteration)
                         : strategy.name;
  frame.microTaskPayloads = payloadsFromStrategy(strategy, planningIntent);

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

