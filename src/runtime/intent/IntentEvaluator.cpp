#include "IntentEvaluator.h"

#include "../ContextExtractor.h"
#include "../impact_analyzer.h"
#include "../../orchestration/IntentDecomposer.h"

#include <external/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ultra::runtime::intent {

namespace {

struct FileMeta {
  std::string nodeId;
  std::string path;
};

struct SymbolMeta {
  std::uint64_t id{0ULL};
  std::string name;
  std::string definedIn;
  std::vector<std::string> usedIn;
  double centrality{0.0};
};

struct ActionMetrics {
  double impact{0.0};
  std::size_t dependencyDepth{0U};
  double centrality{0.0};
  std::size_t tokenEstimate{0U};
  std::set<std::string> impactedFiles;
};

const memory::StateGraph& requireGraph(const GraphSnapshot& snapshot) {
  if (!snapshot.graph) {
    throw std::runtime_error("Intent evaluation requires a non-empty graph snapshot.");
  }
  return *snapshot.graph;
}

double clamp01(const double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, 0.0, 1.0);
}

std::size_t saturatingAdd(const std::size_t left, const std::size_t right) {
  if (std::numeric_limits<std::size_t>::max() - left < right) {
    return std::numeric_limits<std::size_t>::max();
  }
  return left + right;
}

std::string normalizePathToken(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  std::string normalized = value;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  normalized = std::filesystem::path(normalized).lexically_normal().generic_string();
  if (normalized == ".") {
    return {};
  }
  if (normalized.size() >= 2U && normalized[0] == '.' && normalized[1] == '/') {
    normalized.erase(0, 2U);
  }
  return normalized;
}

bool goalTargetsSymbol(const GoalType type) {
  switch (type) {
    case GoalType::ModifySymbol:
    case GoalType::ReduceImpactRadius:
    case GoalType::ImproveCentrality:
    case GoalType::MinimizeTokenUsage:
      return true;
    case GoalType::RefactorModule:
    case GoalType::AddDependency:
    case GoalType::RemoveDependency:
      return false;
  }
  return true;
}

bool actionTargetsFile(const ActionKind kind) {
  switch (kind) {
    case ActionKind::RefactorModule:
    case ActionKind::AddDependency:
    case ActionKind::RemoveDependency:
    case ActionKind::MoveAcrossModules:
      return true;
    case ActionKind::ModifySymbolBody:
    case ActionKind::ReduceImpactRadius:
    case ActionKind::ImproveCentrality:
    case ActionKind::MinimizeTokenUsage:
    case ActionKind::RenameSymbol:
    case ActionKind::ChangeSignature:
    case ActionKind::UpdatePublicAPI:
      return false;
  }
  return false;
}

ActionKind goalToActionKind(const GoalType type) {
  switch (type) {
    case GoalType::ModifySymbol:
      return ActionKind::ModifySymbolBody;
    case GoalType::RefactorModule:
      return ActionKind::RefactorModule;
    case GoalType::ReduceImpactRadius:
      return ActionKind::ReduceImpactRadius;
    case GoalType::ImproveCentrality:
      return ActionKind::ImproveCentrality;
    case GoalType::MinimizeTokenUsage:
      return ActionKind::MinimizeTokenUsage;
    case GoalType::AddDependency:
      return ActionKind::AddDependency;
    case GoalType::RemoveDependency:
      return ActionKind::RemoveDependency;
  }
  return ActionKind::ModifySymbolBody;
}

std::vector<FileMeta> collectFiles(const memory::StateGraph& graph) {
  std::vector<FileMeta> files;
  for (const memory::StateNode& node : graph.queryByType(memory::NodeType::File)) {
    const std::string path = node.data.value("path", std::string{});
    if (path.empty()) {
      continue;
    }
    files.push_back({node.nodeId, path});
  }
  std::sort(files.begin(), files.end(),
            [](const FileMeta& left, const FileMeta& right) {
              return left.path < right.path;
            });
  files.erase(std::unique(files.begin(), files.end(),
                          [](const FileMeta& left, const FileMeta& right) {
                            return left.path == right.path;
                          }),
              files.end());
  return files;
}

std::vector<SymbolMeta> collectSymbols(const memory::StateGraph& graph) {
  std::vector<SymbolMeta> symbols;
  for (const memory::StateNode& node :
       graph.queryByType(memory::NodeType::Symbol)) {
    const std::string name = node.data.value("name", std::string{});
    const std::uint64_t id = node.data.value("symbol_id", 0ULL);
    if (name.empty() || id == 0ULL) {
      continue;
    }

    SymbolMeta symbol;
    symbol.id = id;
    symbol.name = name;
    symbol.definedIn = node.data.value("defined_in", std::string{});
    symbol.centrality = clamp01(node.data.value("centrality", 0.0));
    if (node.data.contains("used_in") && node.data["used_in"].is_array()) {
      for (const auto& usedIn : node.data["used_in"]) {
        if (usedIn.is_string()) {
          symbol.usedIn.push_back(usedIn.get<std::string>());
        }
      }
    }
    std::sort(symbol.usedIn.begin(), symbol.usedIn.end());
    symbol.usedIn.erase(std::unique(symbol.usedIn.begin(), symbol.usedIn.end()),
                        symbol.usedIn.end());
    symbols.push_back(std::move(symbol));
  }

  std::sort(symbols.begin(), symbols.end(),
            [](const SymbolMeta& left, const SymbolMeta& right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.id < right.id;
            });
  return symbols;
}

std::string resolveGoalTarget(const Intent& intent,
                              const std::vector<SymbolMeta>& symbols,
                              const std::vector<FileMeta>& files) {
  const bool symbolGoal = goalTargetsSymbol(intent.goal.type);
  const std::string goalTarget = intent.goal.target;

  if (symbolGoal && !goalTarget.empty()) {
    const auto symbolIt = std::find_if(
        symbols.begin(), symbols.end(),
        [&goalTarget](const SymbolMeta& symbol) {
          return symbol.name == goalTarget;
        });
    if (symbolIt != symbols.end()) {
      return symbolIt->name;
    }
  }

  if (!symbolGoal && !goalTarget.empty()) {
    const std::string normalizedTarget = normalizePathToken(goalTarget);
    for (const FileMeta& file : files) {
      if (file.path == normalizedTarget || file.path == goalTarget) {
        return file.path;
      }
    }
  }

  if (symbolGoal) {
    if (symbols.empty()) {
      return goalTarget;
    }
    std::vector<SymbolMeta> ranked = symbols;
    std::sort(ranked.begin(), ranked.end(),
              [](const SymbolMeta& left, const SymbolMeta& right) {
                if (left.centrality != right.centrality) {
                  return left.centrality > right.centrality;
                }
                if (left.name != right.name) {
                  return left.name < right.name;
                }
                return left.id < right.id;
              });
    return ranked.front().name;
  }

  if (!files.empty()) {
    return files.front().path;
  }
  return normalizePathToken(goalTarget);
}

std::string resolveSecondaryTarget(const std::string& primaryTarget,
                                   const bool symbolGoal,
                                   const std::vector<SymbolMeta>& symbols,
                                   const std::vector<FileMeta>& files) {
  if (symbolGoal) {
    for (const SymbolMeta& symbol : symbols) {
      if (symbol.name != primaryTarget) {
        return symbol.name;
      }
    }
    return primaryTarget;
  }

  for (const FileMeta& file : files) {
    if (file.path != primaryTarget) {
      return file.path;
    }
  }
  return primaryTarget;
}

Action makeAction(const ActionKind kind,
                  const std::string& target,
                  const std::string& details,
                  const std::size_t estimatedFilesChanged,
                  const std::size_t estimatedDependencyDepth,
                  const bool publicApiSurface = false) {
  Action action;
  action.kind = kind;
  action.target = target;
  action.details = details;
  action.estimatedFilesChanged = std::max<std::size_t>(1U, estimatedFilesChanged);
  action.estimatedDependencyDepth =
      std::max<std::size_t>(1U, estimatedDependencyDepth);
  action.publicApiSurface = publicApiSurface;
  return action;
}

Strategy makeTemplate(const std::string& name, const Intent& intent) {
  Strategy strategy;
  strategy.name = name;
  strategy.risk.tolerance = intent.risk;
  strategy.impact.maxFilesConstraint =
      std::max<std::size_t>(1U, intent.constraints.maxFilesChanged);
  strategy.impact.maxDepthConstraint =
      std::max<std::size_t>(1U, intent.constraints.maxImpactDepth);
  strategy.determinism.required = intent.constraints.determinismRequired;
  strategy.tokenCost.budget = std::max<std::size_t>(1U, intent.constraints.tokenBudget);
  strategy.tokenCost.withinBudget = true;
  return strategy;
}

enum class StrategyVariant : std::uint8_t {
  Localized = 0U,
  Balanced = 1U,
  Structural = 2U
};

bool isConcreteTaskId(const std::string& taskId) {
  return taskId == "modify_symbol_body" || taskId == "refactor_module" ||
         taskId == "reduce_impact_radius" || taskId == "improve_centrality" ||
         taskId == "minimize_token_usage" || taskId == "add_dependency" ||
         taskId == "remove_dependency" || taskId == "rename_symbol" ||
         taskId == "change_signature" || taskId == "move_across_modules" ||
         taskId == "update_public_api";
}

ActionKind actionKindForTaskId(const std::string& taskId) {
  if (taskId == "modify_symbol_body") {
    return ActionKind::ModifySymbolBody;
  }
  if (taskId == "refactor_module") {
    return ActionKind::RefactorModule;
  }
  if (taskId == "reduce_impact_radius") {
    return ActionKind::ReduceImpactRadius;
  }
  if (taskId == "improve_centrality") {
    return ActionKind::ImproveCentrality;
  }
  if (taskId == "minimize_token_usage") {
    return ActionKind::MinimizeTokenUsage;
  }
  if (taskId == "add_dependency") {
    return ActionKind::AddDependency;
  }
  if (taskId == "remove_dependency") {
    return ActionKind::RemoveDependency;
  }
  if (taskId == "rename_symbol") {
    return ActionKind::RenameSymbol;
  }
  if (taskId == "change_signature") {
    return ActionKind::ChangeSignature;
  }
  if (taskId == "move_across_modules") {
    return ActionKind::MoveAcrossModules;
  }
  if (taskId == "update_public_api") {
    return ActionKind::UpdatePublicAPI;
  }
  return ActionKind::ModifySymbolBody;
}

bool taskAllowedInVariant(const std::string& taskId,
                          const StrategyVariant variant) {
  if (!isConcreteTaskId(taskId)) {
    return false;
  }

  switch (variant) {
    case StrategyVariant::Localized:
      return taskId != "rename_symbol" && taskId != "change_signature" &&
             taskId != "move_across_modules" && taskId != "update_public_api";
    case StrategyVariant::Balanced:
      return taskId != "move_across_modules" && taskId != "update_public_api";
    case StrategyVariant::Structural:
      return true;
  }
  return false;
}

void appendStrategyActionsFromTasks(Strategy& strategy,
                                    const StrategyVariant variant,
                                    const std::vector<std::string>& orderedTasks,
                                    const Intent& intent,
                                    const std::string& primaryTarget,
                                    const std::string& secondaryTarget,
                                    const std::size_t estimatedFilesChanged,
                                    const std::size_t estimatedDependencyDepth) {
  for (const std::string& taskId : orderedTasks) {
    if (!taskAllowedInVariant(taskId, variant)) {
      continue;
    }

    const ActionKind kind = actionKindForTaskId(taskId);
    const std::string& target =
        kind == ActionKind::MoveAcrossModules ? secondaryTarget : primaryTarget;
    strategy.proposedActions.push_back(makeAction(
        kind, target, "Deterministic plan task: " + taskId + ".",
        estimatedFilesChanged, estimatedDependencyDepth,
        kind == ActionKind::UpdatePublicAPI));
  }

  if (strategy.proposedActions.empty()) {
    strategy.proposedActions.push_back(makeAction(
        goalToActionKind(intent.goal.type), primaryTarget,
        "Deterministic fallback plan task.",
        estimatedFilesChanged, estimatedDependencyDepth));
  }
}

bool lessAction(const Action& left, const Action& right) {
  if (left.kind != right.kind) {
    return static_cast<int>(left.kind) < static_cast<int>(right.kind);
  }
  if (left.target != right.target) {
    return left.target < right.target;
  }
  if (left.details != right.details) {
    return left.details < right.details;
  }
  if (left.estimatedFilesChanged != right.estimatedFilesChanged) {
    return left.estimatedFilesChanged < right.estimatedFilesChanged;
  }
  if (left.estimatedDependencyDepth != right.estimatedDependencyDepth) {
    return left.estimatedDependencyDepth < right.estimatedDependencyDepth;
  }
  return left.publicApiSurface < right.publicApiSurface;
}

ExecutionMode executionModeFor(const RiskTolerance tolerance,
                               const RiskTolerance classification) {
  switch (tolerance) {
    case RiskTolerance::LOW:
      if (classification == RiskTolerance::HIGH) {
        return ExecutionMode::Reject;
      }
      return classification == RiskTolerance::LOW ? ExecutionMode::Direct
                                                  : ExecutionMode::Simulate;
    case RiskTolerance::MEDIUM:
      return classification == RiskTolerance::HIGH ? ExecutionMode::Reject
                                                   : ExecutionMode::Simulate;
    case RiskTolerance::HIGH:
      return ExecutionMode::Direct;
  }
  return ExecutionMode::Reject;
}

std::string decisionReasonFor(const Strategy& strategy,
                              const ExecutionMode mode) {
  switch (mode) {
    case ExecutionMode::Reject:
      return "Rejected by " + toString(strategy.risk.tolerance) +
             " risk tolerance.";
    case ExecutionMode::Simulate:
      return "Requires simulation under " + toString(strategy.risk.tolerance) +
             " risk tolerance.";
    case ExecutionMode::Direct:
      return "Eligible for direct execution under " +
             toString(strategy.risk.tolerance) + " risk tolerance.";
  }
  return "Rejected by risk tolerance.";
}

bool lessPlan(const PlanScore& left, const PlanScore& right) {
  if (left.accepted != right.accepted) {
    return left.accepted && !right.accepted;
  }
  if (left.score != right.score) {
    return left.score > right.score;
  }
  if (left.riskClassification != right.riskClassification) {
    return riskRank(left.riskClassification) < riskRank(right.riskClassification);
  }
  if (left.estimatedImpactRadius != right.estimatedImpactRadius) {
    return left.estimatedImpactRadius < right.estimatedImpactRadius;
  }
  if (left.estimatedTokenUsage != right.estimatedTokenUsage) {
    return left.estimatedTokenUsage < right.estimatedTokenUsage;
  }
  if (left.strategy.impact.dependencyDepth != right.strategy.impact.dependencyDepth) {
    return left.strategy.impact.dependencyDepth <
           right.strategy.impact.dependencyDepth;
  }
  if (left.determinismScore != right.determinismScore) {
    return left.determinismScore > right.determinismScore;
  }
  return left.strategy.name < right.strategy.name;
}

std::map<std::string, std::vector<std::string>> buildReverseDependencies(
    const memory::StateGraph& graph,
    const std::vector<FileMeta>& files) {
  std::map<std::string, std::string> pathByNodeId;
  for (const FileMeta& file : files) {
    pathByNodeId[file.nodeId] = file.path;
  }

  std::map<std::string, std::vector<std::string>> reverse;
  for (const FileMeta& file : files) {
    reverse[file.path] = {};
  }

  for (const FileMeta& file : files) {
    const std::vector<memory::StateEdge> outbound = graph.getOutboundEdges(file.nodeId);
    for (const memory::StateEdge& edge : outbound) {
      const auto toIt = pathByNodeId.find(edge.targetId);
      if (toIt == pathByNodeId.end()) {
        continue;
      }
      reverse[toIt->second].push_back(file.path);
    }
  }

  for (auto& [path, dependents] : reverse) {
    (void)path;
    std::sort(dependents.begin(), dependents.end());
    dependents.erase(std::unique(dependents.begin(), dependents.end()),
                     dependents.end());
  }
  return reverse;
}

void appendJsonStringSet(const nlohmann::json& array, std::set<std::string>& out) {
  if (!array.is_array()) {
    return;
  }
  for (const auto& item : array) {
    if (item.is_string()) {
      out.insert(item.get<std::string>());
    }
  }
}

std::size_t computeDependencyDepth(
    const std::map<std::string, std::vector<std::string>>& reverseDependencies,
    const std::set<std::string>& roots,
    const std::size_t maxDepth) {
  if (roots.empty() || maxDepth == 0U) {
    return 0U;
  }

  std::map<std::string, std::size_t> visitedDepth;
  std::queue<std::pair<std::string, std::size_t>> pending;
  for (const std::string& root : roots) {
    if (reverseDependencies.find(root) == reverseDependencies.end()) {
      continue;
    }
    visitedDepth[root] = 0U;
    pending.push({root, 0U});
  }

  std::size_t deepest = 0U;
  while (!pending.empty()) {
    const auto [path, depth] = pending.front();
    pending.pop();
    deepest = std::max(deepest, depth);
    if (depth >= maxDepth) {
      continue;
    }

    const auto reverseIt = reverseDependencies.find(path);
    if (reverseIt == reverseDependencies.end()) {
      continue;
    }
    for (const std::string& dependent : reverseIt->second) {
      const std::size_t nextDepth = depth + 1U;
      const auto seenIt = visitedDepth.find(dependent);
      if (seenIt != visitedDepth.end() && seenIt->second <= nextDepth) {
        continue;
      }
      visitedDepth[dependent] = nextDepth;
      pending.push({dependent, nextDepth});
    }
  }

  return deepest;
}

double averageCentralityForFiles(
    const std::set<std::string>& files,
    const std::map<std::string, std::vector<double>>& centralityByFile) {
  if (files.empty()) {
    return 0.0;
  }

  double total = 0.0;
  std::size_t count = 0U;
  for (const std::string& file : files) {
    const auto fileIt = centralityByFile.find(file);
    if (fileIt == centralityByFile.end()) {
      continue;
    }
    for (const double value : fileIt->second) {
      total += clamp01(value);
      ++count;
    }
  }
  if (count == 0U) {
    return 0.0;
  }
  return clamp01(total / static_cast<double>(count));
}

std::set<std::string> filesForAction(
    const Action& action,
    const std::map<std::string, SymbolMeta>& symbolByName) {
  std::set<std::string> files;
  if (actionTargetsFile(action.kind)) {
    const std::string normalized = normalizePathToken(action.target);
    if (!normalized.empty()) {
      files.insert(normalized);
    }
    return files;
  }

  const auto symbolIt = symbolByName.find(action.target);
  if (symbolIt == symbolByName.end()) {
    return files;
  }
  if (!symbolIt->second.definedIn.empty()) {
    files.insert(symbolIt->second.definedIn);
  }
  for (const std::string& usedPath : symbolIt->second.usedIn) {
    files.insert(usedPath);
  }
  return files;
}

std::size_t estimateActionTokens(const ContextExtractor& extractor,
                                 const CognitiveState& state,
                                 const Action& action) {
  Query query;
  query.kind = actionTargetsFile(action.kind) ? QueryKind::File : QueryKind::Symbol;
  query.target = action.target;
  query.impactDepth = std::max<std::size_t>(1U, action.estimatedDependencyDepth);

  try {
    return extractor.getMinimalContext(state, query).estimatedTokens;
  } catch (...) {
    const std::size_t detailBudget = action.details.size() / 4U;
    const std::size_t targetBudget = action.target.size() / 4U;
    const std::size_t fileBudget = action.estimatedFilesChanged * 8U;
    return saturatingAdd(32U, saturatingAdd(detailBudget, saturatingAdd(targetBudget, fileBudget)));
  }
}

RiskTolerance classifyRisk(const double riskValue) {
  const double normalized = clamp01(riskValue);
  if (normalized <= 0.33) {
    return RiskTolerance::LOW;
  }
  if (normalized <= 0.66) {
    return RiskTolerance::MEDIUM;
  }
  return RiskTolerance::HIGH;
}

double riskToleranceCeiling(const RiskTolerance tolerance) {
  switch (tolerance) {
    case RiskTolerance::LOW:
      return 0.33;
    case RiskTolerance::MEDIUM:
      return 0.66;
    case RiskTolerance::HIGH:
      return 1.0;
  }
  return 0.66;
}

double determinismPenalty(const ActionKind kind) {
  switch (kind) {
    case ActionKind::RenameSymbol:
      return 0.08;
    case ActionKind::ChangeSignature:
      return 0.10;
    case ActionKind::UpdatePublicAPI:
      return 0.15;
    case ActionKind::MoveAcrossModules:
      return 0.15;
    case ActionKind::AddDependency:
    case ActionKind::RemoveDependency:
      return 0.06;
    case ActionKind::ModifySymbolBody:
    case ActionKind::RefactorModule:
    case ActionKind::ReduceImpactRadius:
    case ActionKind::ImproveCentrality:
    case ActionKind::MinimizeTokenUsage:
      return 0.03;
  }
  return 0.03;
}

ActionMetrics analyzeAction(
    const Action& action,
    const ImpactAnalyzer& impactAnalyzer,
    const ContextExtractor& contextExtractor,
    const CognitiveState& state,
    const std::map<std::string, SymbolMeta>& symbolByName,
    const std::map<std::string, std::vector<double>>& centralityByFile,
    const std::map<std::string, std::vector<std::string>>& reverseDependencies,
    const std::size_t maxDepth,
    const std::size_t maxFilesConstraint) {
  ActionMetrics metrics;

  const bool targetFile = actionTargetsFile(action.kind);
  const std::string impactTarget =
      targetFile ? normalizePathToken(action.target) : action.target;
  const nlohmann::json impactPayload = targetFile
                                           ? impactAnalyzer.analyzeFileImpact(impactTarget,
                                                                              maxDepth)
                                           : impactAnalyzer.analyzeSymbolImpact(impactTarget,
                                                                                maxDepth);

  const double payloadScore = clamp01(impactPayload.value("impact_score", 0.0));
  if (targetFile) {
    appendJsonStringSet(
        impactPayload.value("direct_dependents", nlohmann::json::array()),
        metrics.impactedFiles);
    appendJsonStringSet(
        impactPayload.value("transitive_dependents", nlohmann::json::array()),
        metrics.impactedFiles);
    if (!impactTarget.empty()) {
      metrics.impactedFiles.insert(impactTarget);
    }
  } else {
    appendJsonStringSet(
        impactPayload.value("direct_usage_files", nlohmann::json::array()),
        metrics.impactedFiles);
    appendJsonStringSet(
        impactPayload.value("transitive_impacted_files", nlohmann::json::array()),
        metrics.impactedFiles);
    const std::string definedIn = impactPayload.value("defined_in", std::string{});
    if (!definedIn.empty()) {
      metrics.impactedFiles.insert(definedIn);
    }
  }

  const double countImpact =
      metrics.impactedFiles.empty()
          ? 0.0
          : clamp01(static_cast<double>(metrics.impactedFiles.size()) /
                    static_cast<double>(std::max<std::size_t>(1U, maxFilesConstraint)));
  metrics.impact = clamp01((0.65 * payloadScore) + (0.35 * countImpact));

  const std::set<std::string> targetFiles = filesForAction(action, symbolByName);
  const std::size_t dependencyDepth =
      computeDependencyDepth(reverseDependencies, targetFiles, maxDepth);
  metrics.dependencyDepth = std::max(action.estimatedDependencyDepth, dependencyDepth);

  const auto symbolIt = symbolByName.find(action.target);
  if (symbolIt != symbolByName.end()) {
    metrics.centrality = clamp01(symbolIt->second.centrality);
  } else {
    metrics.centrality = averageCentralityForFiles(metrics.impactedFiles, centralityByFile);
  }

  metrics.tokenEstimate = estimateActionTokens(contextExtractor, state, action);
  return metrics;
}

}  // namespace

std::vector<Strategy> IntentEvaluator::generateStrategies(
    const Intent& intent,
    const CognitiveState& state) const {
  const Intent normalizedIntent = normalizeIntent(intent, state.budget);
  orchestration::IntentDecomposer decomposer;
  const orchestration::TaskGraph executionGraph =
      decomposer.decompose(normalizedIntent);
  std::vector<std::string> orderedTasks = executionGraph.topologicalOrder();
  if (orderedTasks.empty()) {
    orderedTasks.push_back("impact_prediction");
    orderedTasks.push_back("context_extraction");
    orderedTasks.push_back("validate_plan");
  }
  const memory::StateGraph& graph = requireGraph(state.snapshot);
  const std::vector<FileMeta> files = collectFiles(graph);
  const std::vector<SymbolMeta> symbols = collectSymbols(graph);

  const bool symbolGoal = goalTargetsSymbol(normalizedIntent.goal.type);
  const std::string primaryTarget =
      resolveGoalTarget(normalizedIntent, symbols, files);
  const std::string secondaryTarget =
      resolveSecondaryTarget(primaryTarget, symbolGoal, symbols, files);

  const std::size_t maxDepth =
      std::max<std::size_t>(1U, normalizedIntent.constraints.maxImpactDepth);
  const std::size_t maxFiles =
      std::max<std::size_t>(1U, normalizedIntent.constraints.maxFilesChanged);
  const std::size_t localizedFiles = 1U;
  const std::size_t balancedFiles =
      std::max<std::size_t>(1U,
                            std::min<std::size_t>(
                                maxFiles,
                                std::max<std::size_t>(2U, maxFiles / 2U)));
  const std::size_t balancedDepth =
      std::max<std::size_t>(1U, std::min<std::size_t>(maxDepth, 2U));

  Strategy localized = makeTemplate("localized_containment", normalizedIntent);
  appendStrategyActionsFromTasks(localized, StrategyVariant::Localized, orderedTasks,
                                 normalizedIntent, primaryTarget, secondaryTarget,
                                 localizedFiles, 1U);

  Strategy balanced = makeTemplate("bounded_refactor", normalizedIntent);
  appendStrategyActionsFromTasks(balanced, StrategyVariant::Balanced, orderedTasks,
                                 normalizedIntent, primaryTarget, secondaryTarget,
                                 balancedFiles, balancedDepth);

  Strategy structural = makeTemplate("structural_realignment", normalizedIntent);
  appendStrategyActionsFromTasks(structural, StrategyVariant::Structural,
                                 orderedTasks, normalizedIntent, primaryTarget,
                                 secondaryTarget, maxFiles, maxDepth);

  std::vector<Strategy> strategies{localized, balanced, structural};
  for (Strategy& strategy : strategies) {
    std::sort(strategy.proposedActions.begin(), strategy.proposedActions.end(),
              lessAction);
    strategy.proposedActions.erase(
        std::unique(strategy.proposedActions.begin(), strategy.proposedActions.end(),
                    [](const Action& left, const Action& right) {
                      return !lessAction(left, right) && !lessAction(right, left);
                    }),
        strategy.proposedActions.end());
  }

  std::sort(strategies.begin(), strategies.end(),
            [](const Strategy& left, const Strategy& right) {
              return left.name < right.name;
            });
  return strategies;
}

std::vector<PlanScore> IntentEvaluator::evaluateStrategies(
    const std::vector<Strategy>& strategies,
    const CognitiveState& state) const {
  const memory::StateGraph& graph = requireGraph(state.snapshot);
  const std::vector<FileMeta> files = collectFiles(graph);
  const std::vector<SymbolMeta> symbols = collectSymbols(graph);

  std::map<std::string, SymbolMeta> symbolByName;
  std::map<std::string, std::vector<double>> centralityByFile;
  for (const SymbolMeta& symbol : symbols) {
    const auto inserted = symbolByName.emplace(symbol.name, symbol);
    if (!inserted.second && symbol.id < inserted.first->second.id) {
      inserted.first->second = symbol;
    }
    if (!symbol.definedIn.empty()) {
      centralityByFile[symbol.definedIn].push_back(clamp01(symbol.centrality));
    }
    for (const std::string& usedPath : symbol.usedIn) {
      centralityByFile[usedPath].push_back(clamp01(symbol.centrality));
    }
  }
  for (auto& [path, values] : centralityByFile) {
    (void)path;
    std::sort(values.begin(), values.end());
  }

  const std::map<std::string, std::vector<std::string>> reverseDependencies =
      buildReverseDependencies(graph, files);
  ImpactAnalyzer impactAnalyzer(state.snapshot);
  ContextExtractor contextExtractor;

  std::vector<PlanScore> rankedPlans;
  rankedPlans.reserve(strategies.size());
  for (const Strategy& baseStrategy : strategies) {
    Strategy strategy = baseStrategy;
    if (strategy.proposedActions.empty()) {
      PlanScore emptyPlan;
      emptyPlan.strategy = std::move(strategy);
      emptyPlan.score = 0.0;
      emptyPlan.riskClassification = RiskTolerance::HIGH;
      emptyPlan.estimatedTokenUsage = 0U;
      emptyPlan.estimatedImpactRadius = 0.0;
      emptyPlan.determinismScore = 0.0;
      emptyPlan.executionMode = ExecutionMode::Reject;
      emptyPlan.accepted = false;
      emptyPlan.usesImpactPrediction = false;
      emptyPlan.decisionReason = "Rejected because the strategy has no actions.";
      rankedPlans.push_back(std::move(emptyPlan));
      continue;
    }

    const std::size_t maxDepthConstraint =
        std::max<std::size_t>(1U, strategy.impact.maxDepthConstraint);
    const std::size_t maxFilesConstraint =
        std::max<std::size_t>(1U, strategy.impact.maxFilesConstraint);

    std::set<std::string> impactedFiles;
    std::size_t observedDepth = 0U;
    std::size_t estimatedFilesByAction = 0U;
    std::size_t tokenEstimateSum = 0U;
    double impactAccumulator = 0.0;
    double centralityAccumulator = 0.0;
    std::size_t centralityCount = 0U;
    bool touchesPublicApi = false;

    for (const Action& action : strategy.proposedActions) {
      const ActionMetrics metrics = analyzeAction(
          action, impactAnalyzer, contextExtractor, state, symbolByName,
          centralityByFile, reverseDependencies, maxDepthConstraint,
          maxFilesConstraint);
      impactAccumulator += metrics.impact;
      observedDepth = std::max(observedDepth, metrics.dependencyDepth);
      estimatedFilesByAction =
          saturatingAdd(estimatedFilesByAction, action.estimatedFilesChanged);
      tokenEstimateSum = saturatingAdd(tokenEstimateSum, metrics.tokenEstimate);
      impactedFiles.insert(metrics.impactedFiles.begin(), metrics.impactedFiles.end());
      centralityAccumulator += metrics.centrality;
      ++centralityCount;
      touchesPublicApi = touchesPublicApi || action.publicApiSurface ||
                         action.kind == ActionKind::UpdatePublicAPI;
    }

    const std::size_t actionCount = strategy.proposedActions.size();
    const double meanImpact =
        clamp01(impactAccumulator / static_cast<double>(actionCount));
    const std::size_t estimatedFiles = std::max<std::size_t>(
        estimatedFilesByAction, impactedFiles.size());
    const std::size_t estimatedDepth = std::max<std::size_t>(observedDepth, 1U);
    const double averageCentrality =
        centralityCount == 0U
            ? 0.0
            : clamp01(centralityAccumulator / static_cast<double>(centralityCount));

    strategy.impact.radius = meanImpact;
    strategy.impact.estimatedFiles = estimatedFiles;
    strategy.impact.dependencyDepth = estimatedDepth;
    strategy.impact.centrality = averageCentrality;

    const std::size_t meanTokenEstimate = tokenEstimateSum / actionCount;
    const std::size_t actionOverhead = actionCount * 12U;
    strategy.tokenCost.estimatedTokens =
        saturatingAdd(meanTokenEstimate, actionOverhead);
    if (strategy.tokenCost.budget == 0U) {
      strategy.tokenCost.budget = std::max<std::size_t>(1U, state.budget);
    }
    strategy.tokenCost.withinBudget =
        strategy.tokenCost.estimatedTokens <= strategy.tokenCost.budget;

    const double filesRatio =
        clamp01(static_cast<double>(strategy.impact.estimatedFiles) /
                static_cast<double>(maxFilesConstraint));
    const double depthRatio =
        clamp01(static_cast<double>(strategy.impact.dependencyDepth) /
                static_cast<double>(maxDepthConstraint));
    const double tokenRatio =
        clamp01(static_cast<double>(strategy.tokenCost.estimatedTokens) /
                static_cast<double>(strategy.tokenCost.budget));

    const double apiPenalty = touchesPublicApi ? 0.10 : 0.0;
    const double riskValue = clamp01((0.35 * strategy.impact.radius) +
                                     (0.25 * depthRatio) + (0.20 * filesRatio) +
                                     (0.20 * tokenRatio) + apiPenalty);
    strategy.risk.value = riskValue;
    strategy.risk.classification = classifyRisk(riskValue);

    double determinism = 1.0;
    for (const Action& action : strategy.proposedActions) {
      determinism -= determinismPenalty(action.kind);
    }
    if (touchesPublicApi) {
      determinism -= 0.04;
    }
    if (!strategy.tokenCost.withinBudget) {
      determinism -= 0.03;
    }
    if (!strategy.determinism.required) {
      determinism += 0.05;
    }
    strategy.determinism.value = clamp01(determinism);

    double constraintPenalty = 0.0;
    if (strategy.impact.estimatedFiles > maxFilesConstraint) {
      constraintPenalty += 0.12;
    }
    if (strategy.impact.dependencyDepth > maxDepthConstraint) {
      constraintPenalty += 0.10;
    }
    if (!strategy.tokenCost.withinBudget) {
      constraintPenalty += 0.15;
    }
    if (strategy.determinism.required && strategy.determinism.value < 0.80) {
      constraintPenalty += 0.08;
    }

    const double tolerancePenalty =
        std::max(0.0, riskValue - riskToleranceCeiling(strategy.risk.tolerance)) *
        0.35;
    const double baseScore =
        (0.30 * (1.0 - riskValue)) + (0.24 * (1.0 - strategy.impact.radius)) +
        (0.16 * (1.0 - tokenRatio)) + (0.14 * (1.0 - depthRatio)) +
        (0.10 * (1.0 - averageCentrality)) + (0.06 * strategy.determinism.value);
    const double finalScore =
        clamp01(baseScore - constraintPenalty - tolerancePenalty);

    PlanScore plan;
    plan.strategy = std::move(strategy);
    plan.score = finalScore;
    plan.riskClassification = plan.strategy.risk.classification;
    plan.estimatedTokenUsage = plan.strategy.tokenCost.estimatedTokens;
    plan.estimatedImpactRadius = plan.strategy.impact.radius;
    plan.determinismScore = plan.strategy.determinism.value;
    plan.executionMode = executionModeFor(plan.strategy.risk.tolerance,
                                          plan.riskClassification);
    plan.accepted = plan.executionMode != ExecutionMode::Reject;
    plan.usesImpactPrediction = true;
    plan.decisionReason = decisionReasonFor(plan.strategy, plan.executionMode);
    rankedPlans.push_back(std::move(plan));
  }

  std::sort(rankedPlans.begin(), rankedPlans.end(), lessPlan);

  for (std::size_t index = 0U; index < rankedPlans.size(); ++index) {
    rankedPlans[index].rank = index + 1U;
  }
  return rankedPlans;
}

IntentEvaluation IntentEvaluator::evaluateIntent(const Intent& intent,
                                                 const CognitiveState& state) const {
  IntentEvaluation evaluation;
  evaluation.normalizedIntent = normalizeIntent(intent, state.budget);

  orchestration::IntentDecomposer decomposer;
  evaluation.executionGraph = decomposer.decompose(evaluation.normalizedIntent);
  evaluation.orderedTasks = evaluation.executionGraph.topologicalOrder();
  if (evaluation.orderedTasks.empty()) {
    evaluation.orderedTasks.push_back("impact_prediction");
    evaluation.orderedTasks.push_back("context_extraction");
    evaluation.orderedTasks.push_back("validate_plan");
  }
  evaluation.strategies = generateStrategies(evaluation.normalizedIntent, state);
  evaluation.rankedPlans = evaluateStrategies(evaluation.strategies, state);

  for (const PlanScore& plan : evaluation.rankedPlans) {
    if (!plan.accepted) {
      continue;
    }
    evaluation.bestPlan = plan;
    evaluation.hasBestPlan = true;
    break;
  }
  if (!evaluation.hasBestPlan && !evaluation.rankedPlans.empty()) {
    evaluation.bestPlan = evaluation.rankedPlans.front();
  }
  return evaluation;
}

}  // namespace ultra::runtime::intent
