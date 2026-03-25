#include "ExecutionKernel.h"

#include "CognitiveRuntime.h"
#include "tool_router/ToolRouter.h"

#include "../ContextExtractor.h"
#include "../governance/GovernanceEngine.h"
#include "../impact_analyzer.h"
#include "../intent/IntentEvaluator.h"
#include "../../ai/orchestration/MultiModelOrchestrator.h"
#include "../../core/Logger.h"
#include "../../core/state_manager.h"
#include "../../diff/DiffEngine.h"
#include "../../engine/impact/ImpactPredictionEngine.h"
#include "../CPUGovernor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include<iostream>
namespace ultra::runtime {

namespace {

template <typename T>
void sortAndDedupe(std::vector<T>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::vector<std::string> sortedUniqueStrings(std::vector<std::string> values) {
  sortAndDedupe(values);
  return values;
}

std::string normalizePathToken(const std::string& value) {
  if (value.empty()) {
    return {};
  }

  std::string normalized = value;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  normalized =
      std::filesystem::path(normalized).lexically_normal().generic_string();
  if (normalized == ".") {
    return {};
  }
  if (normalized.size() >= 2U && normalized[0] == '.' && normalized[1] == '/') {
    normalized.erase(0, 2U);
  }
  return normalized;
}

std::vector<std::string> normalizeAndSortPaths(std::vector<std::string> values) {
  for (std::string& value : values) {
    value = normalizePathToken(value);
  }
  values.erase(std::remove(values.begin(), values.end(), std::string{}),
               values.end());
  sortAndDedupe(values);
  return values;
}

nlohmann::ordered_json sortJsonKeys(const nlohmann::ordered_json& value) {
  if (value.is_array()) {
    nlohmann::ordered_json sorted = nlohmann::ordered_json::array();
    for (const auto& item : value) {
      sorted.push_back(sortJsonKeys(item));
    }
    return sorted;
  }

  if (!value.is_object()) {
    return value;
  }

  std::vector<std::pair<std::string, nlohmann::ordered_json>> entries;
  entries.reserve(value.size());
  for (auto it = value.begin(); it != value.end(); ++it) {
    entries.emplace_back(it.key(), sortJsonKeys(it.value()));
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  nlohmann::ordered_json sorted = nlohmann::ordered_json::object();
  for (auto& [key, item] : entries) {
    sorted[key] = std::move(item);
  }
  return sorted;
}

const memory::StateGraph& requireGraph(const GraphSnapshot& snapshot) {
  if (!snapshot.graph) {
    throw std::runtime_error("Execution requires a pinned graph snapshot.");
  }
  return *snapshot.graph;
}

const ai::RuntimeState& requireRuntimeState(const GraphSnapshot& snapshot) {
  if (!snapshot.runtimeState) {
    throw std::runtime_error(
        "Execution requires a pinned semantic runtime-state snapshot.");
  }
  return *snapshot.runtimeState;
}

std::string quoteForShell(const std::string& raw) {
  std::string escaped;
  escaped.reserve(raw.size() + 2U);
  escaped.push_back('"');
  for (const char ch : raw) {
    if (ch == '"') {
      escaped += "\\\"";
      continue;
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

std::string slurpTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

struct CommandProbe {
  int exitCode{1};
  std::string output;
};

CommandProbe runCapturedCommand(const std::string& command) {
  CommandProbe probe;

  std::error_code ec;
  const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
  const std::filesystem::path outputPath =
      ec ? std::filesystem::path("ultra_governance_query.log")
         : tempDir /
               ("ultra_governance_query_" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) +
                ".log");

  const std::string redirected =
      command + " > " + quoteForShell(outputPath.string()) + " 2>&1";
  probe.exitCode = std::system(redirected.c_str());
  probe.output = slurpTextFile(outputPath);

  std::error_code removeError;
  std::filesystem::remove(outputPath, removeError);
  return probe;
}

bool hasUltraQueryError(const std::string& output) {
  return output.find("[ERROR]") != std::string::npos;
}

bool isUltraQueryTargetMissing(const std::string& output) {
  return output.find("[UAIR] target not found") != std::string::npos;
}

bool isBinaryBoundaryByte(const unsigned char byte) {
  return byte == 0U || byte < 32U || byte > 126U;
}

std::optional<std::filesystem::path> resolveSymbolsTablePath() {
  const std::filesystem::path preferred =
      "E:/Projects/UltraInfinity.ultra/ai/symbols.tbl";
  const std::filesystem::path fallback =
      "E:/Projects/UltraInfinity/.ultra/ai/symbols.tbl";

  std::error_code ec;
  if (std::filesystem::exists(preferred, ec) && !ec) {
    return preferred;
  }

  ec.clear();
  if (std::filesystem::exists(fallback, ec) && !ec) {
    return fallback;
  }

  return std::nullopt;
}

bool symbolsTableContains(const std::filesystem::path& tablePath,
                          const std::string_view symbol) {
  if (symbol.empty()) {
    return false;
  }

  std::ifstream input(tablePath, std::ios::binary);
  if (!input.is_open()) {
    return false;
  }

  std::vector<char> tableBytes((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
  if (tableBytes.size() < symbol.size()) {
    return false;
  }

  for (std::size_t offset = 0U; offset + symbol.size() <= tableBytes.size(); ++offset) {
    if (!std::equal(symbol.begin(),
                    symbol.end(),
                    tableBytes.begin() + static_cast<std::ptrdiff_t>(offset))) {
      continue;
    }

    const bool beforeBoundary =
        offset == 0U ||
        isBinaryBoundaryByte(static_cast<unsigned char>(tableBytes[offset - 1U]));
    const std::size_t afterOffset = offset + symbol.size();
    const bool afterBoundary =
        afterOffset >= tableBytes.size() ||
        isBinaryBoundaryByte(static_cast<unsigned char>(tableBytes[afterOffset]));
    if (beforeBoundary && afterBoundary) {
      return true;
    }
  }

  return false;
}
std::string joinReasons(const std::vector<std::string>& reasons) {
  if (reasons.empty()) {
    return {};
  }

  std::ostringstream stream;
  for (std::size_t index = 0U; index < reasons.size(); ++index) {
    if (index > 0U) {
      stream << "; ";
    }
    stream << reasons[index];
  }
  return stream.str();
}

RiskLevel toRiskLevel(const double value) {
  if (value >= 0.67) {
    return RiskLevel::High;
  }
  if (value >= 0.34) {
    return RiskLevel::Medium;
  }
  return RiskLevel::Low;
}

RiskLevel toRiskLevel(const intent::RiskTolerance value) {
  switch (value) {
    case intent::RiskTolerance::LOW:
      return RiskLevel::Low;
    case intent::RiskTolerance::MEDIUM:
      return RiskLevel::Medium;
    case intent::RiskTolerance::HIGH:
      return RiskLevel::High;
  }
  return RiskLevel::Medium;
}

std::string toString(const ActionType type) {
  switch (type) {
    case ActionType::Mutation:
      return "Mutation";
    case ActionType::ImpactPrediction:
      return "ImpactPrediction";
    case ActionType::ContextExtraction:
      return "ContextExtraction";
    case ActionType::BranchDiff:
      return "BranchDiff";
    case ActionType::SimulateChange:
      return "SimulateChange";
    case ActionType::IntentEvaluation:
      return "IntentEvaluation";
    case ActionType::ModelGenerate:
      return "ModelGenerate";
    case ActionType::ToolExecution:
      return "ToolExecution";
  }
  return "Mutation";
}

std::string toString(const RiskLevel level) {
  switch (level) {
    case RiskLevel::Low:
      return "LOW";
    case RiskLevel::Medium:
      return "MEDIUM";
    case RiskLevel::High:
      return "HIGH";
    case RiskLevel::Critical:
      return "CRITICAL";
  }
  return "MEDIUM";
}

nlohmann::ordered_json buildIntentJson(const intent::Intent& intentValue) {
  nlohmann::ordered_json payload;
  payload["goal"] = {{"type", intent::toString(intentValue.goal.type)},
                     {"target", intentValue.goal.target}};
  payload["constraints"] = {
      {"branch_scope", intentValue.constraints.branchScope},
      {"determinism_required", intentValue.constraints.determinismRequired},
      {"max_files_changed", intentValue.constraints.maxFilesChanged},
      {"max_impact_depth", intentValue.constraints.maxImpactDepth},
      {"token_budget", intentValue.constraints.tokenBudget},
  };
  payload["risk_tolerance"] = intent::toString(intentValue.risk);
  payload["options"] = {
      {"allow_cross_module_move", intentValue.options.allowCrossModuleMove},
      {"allow_public_api_change", intentValue.options.allowPublicAPIChange},
      {"allow_rename", intentValue.options.allowRename},
      {"allow_signature_change", intentValue.options.allowSignatureChange},
  };
  return payload;
}

nlohmann::ordered_json buildModelExecutionJson(
    const ai::orchestration::OrchestrationContext& orchestrationContext,
    const std::string& providerHint,
    const ai::model::ModelRequest& request,
    const ai::model::ModelResponse& response) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["orchestration_context"] =
      ai::orchestration::toJson(orchestrationContext);
  payload["provider_hint"] = providerHint;
  payload["request"] = ai::model::toJson(request);
  payload["response"] = ai::model::toJson(response);
  return payload;
}

nlohmann::ordered_json buildImpactPredictionJson(
    const engine::impact::ImpactPrediction& prediction) {
  std::vector<engine::impact::ImpactedFile> filesValue = prediction.files;
  for (engine::impact::ImpactedFile& file : filesValue) {
    file.path = normalizePathToken(file.path);
    file.affectedSymbols = sortedUniqueStrings(std::move(file.affectedSymbols));
  }
  std::sort(filesValue.begin(), filesValue.end(),
            [](const engine::impact::ImpactedFile& left,
               const engine::impact::ImpactedFile& right) {
              if (left.path != right.path) {
                return left.path < right.path;
              }
              if (left.depth != right.depth) {
                return left.depth < right.depth;
              }
              if (left.isRoot != right.isRoot) {
                return left.isRoot > right.isRoot;
              }
              return left.affectedSymbols < right.affectedSymbols;
            });

  std::vector<engine::impact::ImpactedSymbol> symbolsValue = prediction.symbols;
  for (engine::impact::ImpactedSymbol& symbol : symbolsValue) {
    symbol.definedIn = normalizePathToken(symbol.definedIn);
  }
  std::sort(symbolsValue.begin(), symbolsValue.end(),
            [](const engine::impact::ImpactedSymbol& left,
               const engine::impact::ImpactedSymbol& right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              if (left.symbolId != right.symbolId) {
                return left.symbolId < right.symbolId;
              }
              if (left.definedIn != right.definedIn) {
                return left.definedIn < right.definedIn;
              }
              if (left.depth != right.depth) {
                return left.depth < right.depth;
              }
              if (left.lineNumber != right.lineNumber) {
                return left.lineNumber < right.lineNumber;
              }
              if (left.isRoot != right.isRoot) {
                return left.isRoot > right.isRoot;
              }
              if (left.publicApi != right.publicApi) {
                return left.publicApi < right.publicApi;
              }
              return left.centrality < right.centrality;
            });

  nlohmann::ordered_json payload;
  payload["kind"] = prediction.targetKind == engine::impact::ImpactTargetKind::File
                        ? "file_impact"
                        : "symbol_impact";
  payload["target"] = prediction.target;

  nlohmann::ordered_json files = nlohmann::ordered_json::array();
  for (const engine::impact::ImpactedFile& file : filesValue) {
    nlohmann::ordered_json item;
    item["affected_symbols"] = file.affectedSymbols;
    item["depth"] = file.depth;
    item["is_root"] = file.isRoot;
    item["path"] = file.path;
    files.push_back(std::move(item));
  }

  nlohmann::ordered_json symbols = nlohmann::ordered_json::array();
  for (const engine::impact::ImpactedSymbol& symbol : symbolsValue) {
    nlohmann::ordered_json item;
    item["centrality"] = symbol.centrality;
    item["defined_in"] = symbol.definedIn;
    item["depth"] = symbol.depth;
    item["is_root"] = symbol.isRoot;
    item["line_number"] = symbol.lineNumber;
    item["name"] = symbol.name;
    item["public_api"] = symbol.publicApi;
    item["symbol_id"] = symbol.symbolId;
    symbols.push_back(std::move(item));
  }

  payload["files"] = std::move(files);
  payload["symbols"] = std::move(symbols);
  payload["affected_files"] = normalizeAndSortPaths(prediction.affectedFiles);
  payload["affected_symbols"] =
      sortedUniqueStrings(prediction.affectedSymbols);
  payload["impact_region"] = normalizeAndSortPaths(prediction.impactRegion);
  payload["risk"] = {
      {"affected_module_count", prediction.risk.affectedModuleCount},
      {"average_centrality", prediction.risk.averageCentrality},
      {"dependency_depth", prediction.risk.dependencyDepth},
      {"public_api_count", prediction.risk.publicApiCount},
      {"score", prediction.risk.score},
      {"score_micros", prediction.risk.scoreMicros},
      {"transitive_impact_size", prediction.risk.transitiveImpactSize},
  };
  return payload;
}

nlohmann::ordered_json buildSimulationJson(
    const engine::impact::SimulationResult& simulation) {
  nlohmann::ordered_json payload = buildImpactPredictionJson(simulation.prediction);
  payload["potential_breakages"] =
      sortedUniqueStrings(simulation.potentialBreakages);
  payload["runtime_state_mutated"] = simulation.runtimeStateMutated;
  return payload;
}

nlohmann::ordered_json buildStrategyJson(const intent::Strategy& strategy) {
  nlohmann::ordered_json payload;
  payload["name"] = strategy.name;
  payload["risk"] = {
      {"classification", intent::toString(strategy.risk.classification)},
      {"tolerance", intent::toString(strategy.risk.tolerance)},
      {"value", strategy.risk.value},
  };
  payload["impact"] = {
      {"centrality", strategy.impact.centrality},
      {"dependency_depth", strategy.impact.dependencyDepth},
      {"estimated_files", strategy.impact.estimatedFiles},
      {"max_depth_constraint", strategy.impact.maxDepthConstraint},
      {"max_files_constraint", strategy.impact.maxFilesConstraint},
      {"radius", strategy.impact.radius},
  };
  payload["determinism"] = {
      {"required", strategy.determinism.required},
      {"value", strategy.determinism.value},
  };
  payload["token_cost"] = {
      {"budget", strategy.tokenCost.budget},
      {"estimated_tokens", strategy.tokenCost.estimatedTokens},
      {"within_budget", strategy.tokenCost.withinBudget},
  };

  nlohmann::ordered_json actions = nlohmann::ordered_json::array();
  for (const intent::Action& action : strategy.proposedActions) {
    nlohmann::ordered_json item;
    item["details"] = action.details;
    item["estimated_dependency_depth"] = action.estimatedDependencyDepth;
    item["estimated_files_changed"] = action.estimatedFilesChanged;
    item["kind"] = intent::toString(action.kind);
    item["public_api_surface"] = action.publicApiSurface;
    item["target"] = action.target;
    actions.push_back(std::move(item));
  }
  payload["actions"] = std::move(actions);
  return payload;
}

nlohmann::ordered_json buildPlanJson(const intent::PlanScore& plan) {
  nlohmann::ordered_json payload;
  payload["accepted"] = plan.accepted;
  payload["decision_reason"] = plan.decisionReason;
  payload["determinism_score"] = plan.determinismScore;
  payload["estimated_impact_radius"] = plan.estimatedImpactRadius;
  payload["estimated_token_usage"] = plan.estimatedTokenUsage;
  payload["execution_mode"] = intent::toString(plan.executionMode);
  payload["rank"] = plan.rank;
  payload["risk_classification"] = intent::toString(plan.riskClassification);
  payload["score"] = plan.score;
  payload["strategy"] = buildStrategyJson(plan.strategy);
  payload["uses_impact_prediction"] = plan.usesImpactPrediction;
  return payload;
}

nlohmann::ordered_json buildGovernanceJson(
    const governance::GovernanceReport& report) {
  std::vector<std::string> violations = report.violations;
  sortAndDedupe(violations);

  nlohmann::ordered_json payload;
  payload["approved"] = report.approved;
  payload["reason"] = report.reason;
  payload["violations"] = violations;
  payload["risk"] = {
      {"classification", intent::toString(report.risk.classification)},
      {"tolerance", intent::toString(report.risk.tolerance)},
      {"value", report.risk.value},
  };
  payload["impact"] = {
      {"centrality", report.impact.centrality},
      {"dependency_depth", report.impact.dependencyDepth},
      {"estimated_files", report.impact.estimatedFiles},
      {"radius", report.impact.radius},
  };
  payload["token_cost"] = {
      {"budget", report.tokenCost.budget},
      {"estimated_tokens", report.tokenCost.estimatedTokens},
      {"within_budget", report.tokenCost.withinBudget},
  };
  payload["determinism"] = {
      {"required", report.determinism.required},
      {"value", report.determinism.value},
  };
  return payload;
}

std::map<std::uint64_t, NodeID> buildSymbolNodeIndex(
    const GraphSnapshot& snapshot) {
  std::map<std::uint64_t, NodeID> nodeIdBySymbolId;
  if (!snapshot.graph) {
    return nodeIdBySymbolId;
  }

  for (const memory::StateNode& node :
       snapshot.graph->queryByType(memory::NodeType::Symbol)) {
    if (!node.data.is_object()) {
      continue;
    }
    const std::uint64_t symbolId = node.data.value("symbol_id", 0ULL);
    if (symbolId == 0ULL) {
      continue;
    }
    const auto inserted = nodeIdBySymbolId.emplace(symbolId, node.nodeId);
    if (!inserted.second && node.nodeId < inserted.first->second) {
      inserted.first->second = node.nodeId;
    }
  }
  return nodeIdBySymbolId;
}

std::map<std::string, NodeID> buildFileNodeIndex(const GraphSnapshot& snapshot) {
  std::map<std::string, NodeID> nodeIdByPath;
  if (!snapshot.graph) {
    return nodeIdByPath;
  }

  for (const memory::StateNode& node :
       snapshot.graph->queryByType(memory::NodeType::File)) {
    if (!node.data.is_object()) {
      continue;
    }
    const std::string path =
        normalizePathToken(node.data.value("path", std::string{}));
    if (path.empty()) {
      continue;
    }
    const auto inserted = nodeIdByPath.emplace(path, node.nodeId);
    if (!inserted.second && node.nodeId < inserted.first->second) {
      inserted.first->second = node.nodeId;
    }
  }
  return nodeIdByPath;
}

std::vector<NodeID> collectImpactNodeIds(
    const engine::impact::ImpactPrediction& prediction,
    const std::map<std::uint64_t, NodeID>& symbolNodeIds,
    const std::map<std::string, NodeID>& fileNodeIds) {
  std::vector<NodeID> impactedNodes;
  for (const engine::impact::ImpactedSymbol& symbol : prediction.symbols) {
    const auto it = symbolNodeIds.find(symbol.symbolId);
    if (it != symbolNodeIds.end()) {
      impactedNodes.push_back(it->second);
    }
  }
  for (const engine::impact::ImpactedFile& file : prediction.files) {
    const auto it = fileNodeIds.find(normalizePathToken(file.path));
    if (it != fileNodeIds.end()) {
      impactedNodes.push_back(it->second);
    }
  }
  sortAndDedupe(impactedNodes);
  return impactedNodes;
}

std::vector<NodeID> collectContextNodeIds(
    const std::vector<SymbolID>& includedNodes,
    const std::map<std::uint64_t, NodeID>& symbolNodeIds) {
  std::vector<NodeID> nodeIds;
  for (const SymbolID symbolId : includedNodes) {
    const auto it = symbolNodeIds.find(symbolId);
    if (it != symbolNodeIds.end()) {
      nodeIds.push_back(it->second);
    }
  }
  sortAndDedupe(nodeIds);
  return nodeIds;
}

void collectPathStringsRecursive(const nlohmann::ordered_json& value,
                                 const std::string& key,
                                 std::set<std::string>& paths) {
  if (value.is_string()) {
    const bool pathKey =
        key == "affected_files" || key == "defined_in" || key == "direct_dependents" ||
        key == "direct_usage_files" || key == "file_path" || key == "impact_region" ||
        key == "path" || key == "source_path" || key == "target_path" ||
        key == "transitive_dependents" || key == "transitive_impacted_files";
    if (!pathKey) {
      return;
    }
    const std::string normalized = normalizePathToken(value.get<std::string>());
    if (!normalized.empty()) {
      paths.insert(normalized);
    }
    return;
  }

  if (value.is_array()) {
    for (const auto& item : value) {
      collectPathStringsRecursive(item, key, paths);
    }
    return;
  }

  if (!value.is_object()) {
    return;
  }

  for (auto it = value.begin(); it != value.end(); ++it) {
    collectPathStringsRecursive(it.value(), it.key(), paths);
  }
}

std::vector<std::string> collectNormalizedPaths(
    const nlohmann::ordered_json& payload) {
  std::set<std::string> paths;
  collectPathStringsRecursive(payload, std::string{}, paths);
  return std::vector<std::string>(paths.begin(), paths.end());
}

bool isKnownSymbolTarget(const GraphSnapshot& snapshot,
                         const std::string& target) {
  if (!snapshot.graph) {
    return false;
  }
  for (const memory::StateNode& node :
       snapshot.graph->queryByType(memory::NodeType::Symbol)) {
    if (!node.data.is_object()) {
      continue;
    }
    if (node.data.value("name", std::string{}) == target) {
      return true;
    }
  }
  return false;
}

const std::map<std::string, std::vector<std::string>>& fallbackToolInputParams() {
  static const std::map<std::string, std::vector<std::string>> kToolInputs = {
      {"read_file", {"path"}},
      {"write_file", {"path", "content"}},
      {"append_file", {"path", "content"}},
      {"delete_file", {"path"}},
      {"list_dir", {}},
      {"search_files", {"pattern"}},
      {"run_command", {"command"}},
  };
  return kToolInputs;
}

bool isNativeToolRouterTool(const std::string& toolName) {
  return fallbackToolInputParams().find(toolName) !=
         fallbackToolInputParams().end();
}

bool requiresModelGeneration(const intent::ActionKind kind) {
  switch (kind) {
    case intent::ActionKind::ModifySymbolBody:
    case intent::ActionKind::RefactorModule:
    case intent::ActionKind::AddDependency:
    case intent::ActionKind::RemoveDependency:
    case intent::ActionKind::RenameSymbol:
    case intent::ActionKind::ChangeSignature:
    case intent::ActionKind::UpdatePublicAPI:
    case intent::ActionKind::MoveAcrossModules:
      return true;
    case intent::ActionKind::ReduceImpactRadius:
    case intent::ActionKind::ImproveCentrality:
    case intent::ActionKind::MinimizeTokenUsage:
      return false;
  }
  return false;
}

ai::orchestration::TaskType taskTypeForStrategyAction(
    const intent::Action& strategyAction) {
  switch (strategyAction.kind) {
    case intent::ActionKind::ModifySymbolBody:
    case intent::ActionKind::RefactorModule:
    case intent::ActionKind::AddDependency:
    case intent::ActionKind::RemoveDependency:
    case intent::ActionKind::RenameSymbol:
    case intent::ActionKind::ChangeSignature:
    case intent::ActionKind::UpdatePublicAPI:
    case intent::ActionKind::MoveAcrossModules:
      return ai::orchestration::TaskType::Coding;
    case intent::ActionKind::ReduceImpactRadius:
    case intent::ActionKind::ImproveCentrality:
    case intent::ActionKind::MinimizeTokenUsage:
      return ai::orchestration::TaskType::Analysis;
  }
  return ai::orchestration::TaskType::Analysis;
}

std::string selectProviderForAction(const intent::Action& strategyAction) {
  if (strategyAction.publicApiSurface ||
      strategyAction.kind == intent::ActionKind::ChangeSignature ||
      strategyAction.kind == intent::ActionKind::UpdatePublicAPI) {
    return "deepseek";
  }
  return "ollama";
}

ai::model::ModelRequest buildModelRequestForStrategyAction(
    const intent::Action& strategyAction,
    const CognitiveState& state) {
  ai::model::ModelRequest request;
  request.systemPrompt =
      "You are UltraInfinity coding execution stage. Generate deterministic "
      "code-level changes and actionable implementation output.";

  std::ostringstream prompt;
  prompt << "Action Kind: " << intent::toString(strategyAction.kind) << "\n";
  prompt << "Target: " << strategyAction.target << "\n";
  prompt << "Details: " << strategyAction.details << "\n";
  prompt << "Estimated Files Changed: " << strategyAction.estimatedFilesChanged
         << "\n";
  prompt << "Estimated Dependency Depth: "
         << strategyAction.estimatedDependencyDepth << "\n";
  prompt << "Branch: " << state.snapshot.branch.toString() << "\n";
  prompt << "Snapshot Version: " << state.snapshot.version << "\n";
  prompt << "Return concrete implementation content suitable for deterministic "
            "execution.";
  request.prompt = prompt.str();

  request.temperature = 0.10;
  request.maxTokens = std::max<std::size_t>(
      512U,
      strategyAction.estimatedFilesChanged *
          std::max<std::size_t>(256U, strategyAction.estimatedDependencyDepth * 128U));

  request.toolsAvailable = {
      "read_file",
      "write_file",
      "append_file",
      "delete_file",
      "list_dir",
      "search_files",
      "run_command",
  };

  request.contextPayload = {
      {"action_kind", intent::toString(strategyAction.kind)},
      {"target", strategyAction.target},
      {"details", strategyAction.details},
      {"estimated_files_changed", strategyAction.estimatedFilesChanged},
      {"estimated_dependency_depth", strategyAction.estimatedDependencyDepth},
      {"branch", state.snapshot.branch.toString()},
      {"snapshot_version", state.snapshot.version},
  };
  return request;
}

ai::orchestration::OrchestrationContext buildModelOrchestrationForStrategyAction(
    const intent::Action& strategyAction,
    const ai::model::ModelRequest& request) {
  ai::orchestration::OrchestrationContext context;
  context.taskType = taskTypeForStrategyAction(strategyAction);
  context.complexity =
      (strategyAction.estimatedDependencyDepth >= 3U ||
       strategyAction.estimatedFilesChanged >= 4U)
          ? ai::orchestration::TaskComplexity::High
          : ai::orchestration::TaskComplexity::Medium;
  context.priority = strategyAction.publicApiSurface
                         ? ai::orchestration::TaskPriority::Urgent
                         : ai::orchestration::TaskPriority::Standard;
  context.tokenBudget = request.maxTokens;
  context.availableModels = {
      selectProviderForAction(strategyAction),
      "ollama",
      "gemini",
      "openai",
      "anthropic",
      "deepseek",
  };
  return context;
}

Action strategyActionToKernelAction(const intent::Action& strategyAction,
                                    const CognitiveState& state) {
  Action action;
  action.id = intent::toString(strategyAction.kind) + ":" + strategyAction.target;
  action.target = strategyAction.target.empty() ? "workspace_root" : strategyAction.target;
  action.branch = state.snapshot.branch.toString();
  action.snapshotVersion = state.snapshot.version;

  if (strategyAction.kind == intent::ActionKind::ReduceImpactRadius ||
      strategyAction.kind == intent::ActionKind::ImproveCentrality) {
    action.type = ActionType::ImpactPrediction;
    return action;
  }

  if (strategyAction.kind == intent::ActionKind::MinimizeTokenUsage) {
    action.type = ActionType::ContextExtraction;
    return action;
  }

  if (requiresModelGeneration(strategyAction.kind)) {
    action.type = ActionType::ModelGenerate;
    action.modelProvider = selectProviderForAction(strategyAction);
    action.modelRequest = buildModelRequestForStrategyAction(strategyAction, state);
    action.orchestrationContext =
        buildModelOrchestrationForStrategyAction(strategyAction, *action.modelRequest);
    return action;
  }

  action.type = ActionType::SimulateChange;
  return action;
}


ai::orchestration::OrchestrationContext buildOrchestrationContext(
    const Action& action) {
  ai::orchestration::OrchestrationContext context =
      action.orchestrationContext.value_or(
          ai::orchestration::OrchestrationContext{});
  if (context.tokenBudget == 0U && action.modelRequest.has_value()) {
    context.tokenBudget = action.modelRequest->maxTokens;
  }
  if (!action.modelProvider.empty() && context.availableModels.empty()) {
    context.availableModels = {action.modelProvider};
  }
  return context;
}

}  // namespace

ExecutionKernel::ExecutionKernel(
    core::StateManager& stateManager,
    std::shared_ptr<ai::orchestration::IMultiModelOrchestrator> modelOrchestrator)
    : stateManager_(stateManager),
      modelOrchestrator_(modelOrchestrator != nullptr
                             ? std::move(modelOrchestrator)
                             : ai::orchestration::MultiModelOrchestrator::
                                   createDefault(stateManager.projectRoot())) {}

RiskLevel ExecutionKernel::maxRisk(const RiskLevel left,
                                   const RiskLevel right) noexcept {
  return static_cast<int>(right) > static_cast<int>(left) ? right : left;
}

GovernanceDecision ExecutionKernel::evaluate_action(
    const std::string& tool,
    const std::map<std::string, std::string>& args) {
  GovernanceDecision decision;
  decision.allowed = true;
  decision.risk = RiskLevel::Low;
  decision.confidence = 0.92F;

  std::vector<std::string> reasons;
  std::size_t unknownParameterCount = 0U;

  const cognitive::tools::ToolDefinition* definition =
      toolExecutor_.registry().get_tool(tool);
  std::vector<std::string> requiredArgs;
  if (definition != nullptr) {
    requiredArgs = definition->input_params;
  } else {
    const auto fallbackTool = fallbackToolInputParams().find(tool);
    if (fallbackTool != fallbackToolInputParams().end()) {
      requiredArgs = fallbackTool->second;
    } else {
      decision.allowed = false;
      decision.risk = RiskLevel::Critical;
      decision.confidence = 0.0F;
      decision.reason = "Unknown tool: " + tool;
      core::Logger::warning(core::LogCategory::General,
                            "Governance blocked unknown tool '" + tool + "'.");
      return decision;
    }
  }

  if (tool == "query_symbol" || tool == "read_source" ||
      tool == "read_file" || tool == "list_dir" ||
      tool == "search_files") {
    decision.risk = RiskLevel::Low;
  } else if (tool == "impact_analysis" || tool == "get_context" ||
             tool == "get_status") {
    decision.risk = RiskLevel::Medium;
  } else if (tool == "write_file" || tool == "append_file" ||
             tool == "delete_file" || tool == "run_command") {
    decision.risk = RiskLevel::High;
    reasons.push_back("Mutable tool execution requires strict governance.");
  } else {
    decision.risk = RiskLevel::Critical;
    reasons.push_back("Tool is outside deterministic governance policy");
  }

  if (!requiredArgs.empty() && args.empty()) {
    decision.risk = maxRisk(decision.risk, RiskLevel::Critical);
    decision.confidence -= 0.70F;
    reasons.push_back("Required arguments are empty");
  }

  for (const std::string& required : requiredArgs) {
    const auto it = args.find(required);
    if (it == args.end() || it->second.empty()) {
      decision.risk = maxRisk(decision.risk, RiskLevel::Critical);
      decision.confidence -= 0.35F;
      reasons.push_back("Missing required argument: " + required);
    }
  }

  for (const auto& [key, value] : args) {
    if (value.empty()) {
      decision.risk = maxRisk(decision.risk, RiskLevel::Critical);
      decision.confidence -= 0.20F;
      reasons.push_back("Argument '" + key + "' is empty");
      continue;
    }

    const bool known =
        std::find(requiredArgs.begin(), requiredArgs.end(), key) != requiredArgs.end();
    if (!known) {
      ++unknownParameterCount;
      decision.risk = maxRisk(decision.risk, RiskLevel::High);
      decision.confidence -= 0.15F;
      reasons.push_back("Unknown parameter: " + key);
    }
  }

  if (tool == "query_symbol" || tool == "impact_analysis") {
    const auto symbolsTablePath = resolveSymbolsTablePath();
    if (!symbolsTablePath.has_value()) {
      decision.risk = maxRisk(decision.risk, RiskLevel::Critical);
      decision.confidence -= 0.35F;
      reasons.push_back("symbols.tbl is unavailable for validation");
    }

    const auto targetIt = args.find("target");
    if (targetIt != args.end() && !targetIt->second.empty() &&
        !validateSymbolWithUltra(targetIt->second)) {
      decision.risk = maxRisk(decision.risk, RiskLevel::Critical);
      decision.confidence -= 0.70F;
      reasons.push_back("Target symbol is not indexed: " + targetIt->second);
    }
  }

  const std::size_t historyFailures =
      std::max(toolExecutor_.consecutive_failures(tool),
               toolFailureCounts_[tool]);
  if (historyFailures >= 2U) {
    decision.risk = maxRisk(decision.risk, RiskLevel::High);
    decision.confidence -=
        std::min(0.30F, 0.10F * static_cast<float>(historyFailures));
    reasons.push_back("Repeated failures detected for tool execution");
  }

  CognitiveRuntime runtime(stateManager_);
  const GovernanceSignal signal = runtime.currentGovernanceSignal();
  const bool overloaded =
      !signal.idle &&
      signal.activeWorkloads >= std::max<std::size_t>(1U, signal.recommendedThreads);
  if (overloaded) {
    decision.risk = maxRisk(decision.risk, RiskLevel::High);
    decision.confidence -= 0.25F;
    reasons.push_back("CPUGovernor reports overloaded execution environment");
  }
  if (signal.movingAverageMs >= 120.0) {
    decision.risk = maxRisk(decision.risk, RiskLevel::High);
    decision.confidence -= 0.10F;
    reasons.push_back("CPUGovernor moving-average latency is elevated");
  }
  if (signal.idle) {
    decision.confidence = std::min(1.0F, decision.confidence + 0.05F);
  }

  decision.confidence = std::clamp(decision.confidence, 0.0F, 1.0F);

  if (decision.confidence < 0.4F) {
    decision.allowed = false;
    reasons.push_back("Confidence dropped below governance threshold");
  }

  if (decision.risk == RiskLevel::Critical) {
    decision.allowed = false;
  }

  if (decision.risk == RiskLevel::High &&
      (unknownParameterCount > 0U || historyFailures >= 2U || overloaded)) {
    decision.allowed = false;
  }

  decision.reason = joinReasons(reasons);
  if (decision.reason.empty()) {
    decision.reason =
        decision.allowed ? "Governance approved action." : "Governance blocked action.";
  }

  core::Logger::info(
      core::LogCategory::General,
      "Governance decision tool='" + tool + "' risk=" +
          toString(decision.risk) + " confidence=" +
          std::to_string(decision.confidence) + " allowed=" +
          (decision.allowed ? "true" : "false") + " reason='" +
          decision.reason + "'.");

  return decision;
}

std::string ExecutionKernel::stableActionId(const Action& action) const {
  if (!action.id.empty()) {
    return action.id;
  }

  std::string id = toString(action.type);
  if (!action.target.empty()) {
    id += ":" + action.target;
  }
  if (!action.toolName.empty()) {
    id += ":tool:" + action.toolName;
    for (const auto& [key, value] : action.toolArgs) {
      id += ":" + key + "=" + value;
    }
  }
  if (action.intentRequest.has_value()) {
    id += ":intent:" + intent::toString(action.intentRequest->goal.type) + ":" +
          action.intentRequest->goal.target;
  }
  if (action.orchestrationContext.has_value()) {
    id += ":task:" +
          ai::orchestration::toString(action.orchestrationContext->taskType);
    id += ":complexity:" +
          ai::orchestration::toString(action.orchestrationContext->complexity);
    id += ":priority:" +
          ai::orchestration::toString(action.orchestrationContext->priority);
    id += ":latency_ms:" +
          std::to_string(action.orchestrationContext->latencyBudgetMs);
    id += ":token_budget:" +
          std::to_string(action.orchestrationContext->tokenBudget);
    const std::vector<std::string> models =
        ai::orchestration::normalizedAvailableModels(
            *action.orchestrationContext);
    if (!models.empty()) {
      id += ":available_models:";
      for (const std::string& model : models) {
        id += model + ",";
      }
    }
  }
  if (!action.modelProvider.empty()) {
    id += ":provider_hint:" + action.modelProvider;
  }
  if (action.modelRequest.has_value()) {
    id += ":prompt_length:" + std::to_string(action.modelRequest->prompt.size());
    id += ":max_tokens:" + std::to_string(action.modelRequest->maxTokens);
  }
  if (!action.branch.empty()) {
    id += ":branch:" + action.branch;
  }
  id += ":v" + std::to_string(action.snapshotVersion);
  return id;
}

void ExecutionKernel::validateAction(const Action& action,
                                     const CognitiveState& state) const {
  if (action.snapshotVersion != 0U &&
      action.snapshotVersion != state.snapshot.version) {
    throw std::runtime_error(
        "Execution action snapshot version does not match pinned state.");
  }
  if (!action.branch.empty() &&
      action.branch != state.snapshot.branch.toString()) {
    throw std::runtime_error(
        "Execution action branch does not match pinned state.");
  }

  switch (action.type) {
    case ActionType::Mutation:
      if (!action.mutation) {
        throw std::runtime_error("Execution action is missing mutation callback.");
      }
      return;
    case ActionType::ImpactPrediction:
    case ActionType::ContextExtraction:
    case ActionType::SimulateChange:
      if (action.target.empty()) {
        throw std::runtime_error("Execution action target is empty.");
      }
      return;
    case ActionType::BranchDiff:
      if (!action.comparisonSnapshot.has_value()) {
        throw std::runtime_error(
            "Branch diff execution requires a comparison snapshot.");
      }
      return;
    case ActionType::IntentEvaluation:
      if (!action.intentRequest.has_value()) {
        throw std::runtime_error(
            "Intent execution requires an intent payload.");
      }
      return;
    case ActionType::ModelGenerate:
      if (!action.modelRequest.has_value()) {
        throw std::runtime_error(
            "Model generation requires a model request payload.");
      }
      if (action.modelRequest->prompt.empty()) {
        throw std::runtime_error("Model generation prompt is empty.");
      }
      if (!action.orchestrationContext.has_value() && action.modelProvider.empty()) {
        throw std::runtime_error(
            "Model generation requires an orchestration context or provider hint.");
      }
      return;
    case ActionType::ToolExecution:
      if (action.toolName.empty()) {
        throw std::runtime_error("Tool execution requires a registered tool name.");
      }
      return;
  }
}

void ExecutionKernel::sortOutputs(Result& result) const {
  sortAndDedupe(result.impactedNodes);
  sortAndDedupe(result.normalizedPaths);
  if (!result.payload.is_null()) {
    result.payload = sortJsonKeys(result.payload);
  }
}

Result ExecutionKernel::executeIntent(const intent::Intent& intentValue,
                                      const CognitiveState& state,
                                      const governance::Policy& policy) {
  Action action;
  action.type = ActionType::IntentEvaluation;
  action.id = "intent:" + intent::toString(intentValue.goal.type) + ":" +
              intentValue.goal.target;
  action.target = intentValue.goal.target;
  action.branch = state.snapshot.branch.toString();
  action.snapshotVersion = state.snapshot.version;
  action.intentRequest = intentValue;
  action.policy = policy;
  return execute(action, state);
}

Result ExecutionKernel::executeActionLocked(const Action& action,
                                            const CognitiveState& state) {
  Result result;
  result.type = action.type;
  result.previousVersion = state.snapshot.version;
  result.resultingVersion = state.snapshot.version;
  try {
    result.previousHash = state.snapshot.deterministicHash();
    result.resultingHash = result.previousHash;
  } catch (...) {
  }

  const std::map<std::uint64_t, NodeID> symbolNodeIds =
      buildSymbolNodeIndex(state.snapshot);
  const std::map<std::string, NodeID> fileNodeIds =
      buildFileNodeIndex(state.snapshot);

  switch (action.type) {
    case ActionType::Mutation: {
      const core::KernelMutationOutcome outcome =
          stateManager_.applyOverlayMutation(state.snapshot, action.mutation);
      result.applied = outcome.applied;
      result.rolledBack = outcome.rolledBack;
      result.previousVersion = outcome.versionBefore;
      result.resultingVersion = outcome.versionAfter;
      result.previousHash = outcome.hashBefore;
      result.resultingHash = outcome.hashAfter;
      result.ok = outcome.applied;
      result.message = outcome.message.empty()
                           ? (outcome.applied ? "Mutation committed."
                                              : "Mutation was rejected.")
                           : outcome.message;
      result.risk = toRiskLevel(action.riskScore);
      break;
    }

    case ActionType::ImpactPrediction: {
      const ai::RuntimeState& runtimeState = requireRuntimeState(state.snapshot);
      engine::impact::ImpactPredictionEngine engine(
          runtimeState, state.snapshot.graphStore, state.snapshot.version);
      const bool symbolTarget = isKnownSymbolTarget(state.snapshot, action.target);
      const engine::impact::ImpactPrediction prediction =
          symbolTarget ? engine.predictSymbolImpact(action.target)
                       : engine.predictFileImpact(action.target);
      result.payload = buildImpactPredictionJson(prediction);
      result.impactedNodes =
          collectImpactNodeIds(prediction, symbolNodeIds, fileNodeIds);
      result.normalizedPaths = collectNormalizedPaths(result.payload);
      result.risk = toRiskLevel(prediction.risk.score);
      result.ok = true;
      result.message = "Impact prediction completed.";
      break;
    }

    case ActionType::ContextExtraction: {
      ContextExtractor extractor;
      Query query;
      query.kind = QueryKind::Auto;
      query.target = action.target;
      query.impactDepth = 2U;
      const ContextSlice slice = extractor.getMinimalContext(state, query);
      result.payload = nlohmann::ordered_json::parse(slice.json);
      result.impactedNodes = collectContextNodeIds(slice.includedNodes, symbolNodeIds);
      result.normalizedPaths = collectNormalizedPaths(result.payload);
      const double utilization =
          state.budget == 0U
              ? 0.0
              : static_cast<double>(slice.estimatedTokens) /
                    static_cast<double>(state.budget);
      result.risk = toRiskLevel(utilization);
      result.ok = true;
      result.message = "Context extraction completed.";
      break;
    }

    case ActionType::BranchDiff: {
      const memory::StateSnapshot currentSnapshot =
          requireGraph(state.snapshot).snapshot(state.snapshot.version);
      result.payload = diff::DiffEngine::diffBranchesJson(
          currentSnapshot, *action.comparisonSnapshot);
      result.normalizedPaths = collectNormalizedPaths(result.payload);
      for (const std::string& path : result.normalizedPaths) {
        const auto it = fileNodeIds.find(path);
        if (it != fileNodeIds.end()) {
          result.impactedNodes.push_back(it->second);
        }
      }
      result.risk = toRiskLevel(static_cast<double>(result.normalizedPaths.size()) /
                                10.0);
      result.ok = true;
      result.message = "Branch diff completed.";
      break;
    }

    case ActionType::SimulateChange: {
      // DEBUG — if you see this, strategyActionToKernelAction mapped to SimulateChange
      std::cerr << "[ULTRA-DEBUG] SimulateChange entered. target=" << action.target << "\n";
      const ai::RuntimeState& runtimeState = requireRuntimeState(state.snapshot);
      engine::impact::ImpactPredictionEngine engine(
          runtimeState, state.snapshot.graphStore, state.snapshot.version);
      const bool symbolTarget = isKnownSymbolTarget(state.snapshot, action.target);
      const engine::impact::SimulationResult simulation =
          symbolTarget ? engine.simulateSymbolChange(action.target)
                       : engine.simulateFileChange(action.target);
      result.payload = buildSimulationJson(simulation);
      result.impactedNodes = collectImpactNodeIds(
          simulation.prediction, symbolNodeIds, fileNodeIds);
      result.normalizedPaths = collectNormalizedPaths(result.payload);
      result.risk = toRiskLevel(simulation.prediction.risk.score);
      result.ok = true;
      result.message = "Change simulation completed.";
      break;
    }

    case ActionType::ModelGenerate: {
      const ai::orchestration::OrchestrationContext orchestrationContext =
          buildOrchestrationContext(action);

      // DEBUG — printed to stderr so it doesn't pollute the JSON stdout
      std::cerr << "[ULTRA-DEBUG] ModelGenerate entered."
                << " provider=" << action.modelProvider
                << " prompt_len=" << (action.modelRequest.has_value()
                                          ? action.modelRequest->prompt.size()
                                          : 0U)
                << " orchestrator="
                << (modelOrchestrator_ != nullptr ? "set" : "NULL")
                << "\n";

      if (modelOrchestrator_ == nullptr) {
        ai::model::ModelResponse response;
        response.ok = false;
        response.errorCode = ai::model::ModelErrorCode::ProviderUnavailable;
        response.errorMessage = "Model orchestrator is unavailable.";
        result.payload = buildModelExecutionJson(orchestrationContext,
                                                 action.modelProvider,
                                                 *action.modelRequest, response);
        result.normalizedPaths = collectNormalizedPaths(result.payload);
        result.risk = RiskLevel::Medium;
        result.ok = false;
        result.message = response.errorMessage;
        break;
      }

      const ai::model::ModelResponse response =
          modelOrchestrator_->generate(*action.modelRequest, orchestrationContext);

      // DEBUG
      std::cerr << "[ULTRA-DEBUG] ModelGenerate response."
                << " ok=" << (response.ok ? "true" : "false")
                << " error=" << response.errorMessage
                << " output_len=" << response.textOutput.size()
                << "\n";

      // DUMP model response to file for inspection
      {
        std::ofstream debugFile("C:/Temp/ultra_model_response.txt",
                                std::ios::binary | std::ios::trunc);
        if (debugFile.is_open()) {
          debugFile << "=== ModelGenerate Response ===\n";
          debugFile << "ok: " << (response.ok ? "true" : "false") << "\n";
          debugFile << "error: " << response.errorMessage << "\n";
          debugFile << "output_len: " << response.textOutput.size() << "\n";
          debugFile << "=== text_output ===\n";
          debugFile << response.textOutput << "\n";
          debugFile << "=== payload json ===\n";
          // Also dump what toJson produces so we can see the key name
          debugFile << ai::model::toJson(response).dump(2) << "\n";
        }
      }

      result.payload = buildModelExecutionJson(orchestrationContext,
                                               action.modelProvider,
                                               *action.modelRequest, response);
      result.normalizedPaths = collectNormalizedPaths(result.payload);
      result.risk = response.ok ? RiskLevel::Low : RiskLevel::Medium;
      result.ok = response.ok;
      result.message = response.ok ? "Model generation completed."
                                   : (response.errorMessage.empty()
                                          ? "Model generation failed."
                                          : response.errorMessage);
      break;
    }

    case ActionType::ToolExecution: {
      const GovernanceDecision decision =
          evaluate_action(action.toolName, action.toolArgs);
      result.payload = {
          {"args", action.toolArgs},
          {"governance",
           {{"allowed", decision.allowed},
            {"confidence", decision.confidence},
            {"reason", decision.reason},
            {"risk", toString(decision.risk)}}},
          {"tool", action.toolName},
      };

      result.risk = decision.risk;
      if (!decision.allowed) {
        result.ok = false;
        result.message = "Governance blocked tool execution: " + decision.reason;
        result.normalizedPaths = collectNormalizedPaths(result.payload);
        break;
      }

      std::string toolOutput;
      bool toolSucceeded = false;

      if (toolExecutor_.registry().get_tool(action.toolName) != nullptr) {
        toolOutput = toolExecutor_.execute(action.toolName, action.toolArgs);
        toolSucceeded = toolExecutor_.last_execution_succeeded();
      } else if (isNativeToolRouterTool(action.toolName)) {
        cognitive::tool_router::ToolRouter directRouter;
        toolOutput = directRouter.route_and_execute(action.toolName, action.toolArgs);
        toolSucceeded = toolOutput.rfind("ERROR:", 0U) != 0U &&
                        toolOutput.rfind("[ERROR]", 0U) != 0U;
        result.payload["router_fallback"] = true;
      } else {
        toolOutput = "ERROR: tool '" + action.toolName + "' is not registered.";
      }

      result.payload["output"] = toolOutput;
      result.normalizedPaths = collectNormalizedPaths(result.payload);
      result.ok = toolSucceeded;
      result.risk = result.ok ? decision.risk : maxRisk(decision.risk, RiskLevel::High);
      result.message =
          result.ok ? "Tool execution completed deterministically." : toolOutput;
      break;

    }

    case ActionType::IntentEvaluation: {
      intent::IntentEvaluator evaluator;
      const intent::IntentEvaluation evaluation =
          evaluator.evaluateIntent(*action.intentRequest, state);
      result.payload["intent"] = buildIntentJson(evaluation.normalizedIntent);
      result.payload["ordered_tasks"] = evaluation.orderedTasks;

      nlohmann::ordered_json plans = nlohmann::ordered_json::array();
      for (const intent::PlanScore& plan : evaluation.rankedPlans) {
        plans.push_back(buildPlanJson(plan));
      }
      result.payload["ranked_plans"] = std::move(plans);

      if (!evaluation.hasBestPlan) {
        result.risk = RiskLevel::High;
        result.ok = false;
        result.message = "No acceptable intent plan was produced.";
        break;
      }

      result.payload["best_plan"] = buildPlanJson(evaluation.bestPlan);
      governance::GovernanceEngine governance(&stateManager_.cognitiveMemory());
      const governance::Policy policy =
          action.policy.value_or(governance::Policy{});
      const governance::GovernanceReport governanceReport =
          governance.evaluate(evaluation.bestPlan.strategy, policy, state);
      result.payload["governance"] = buildGovernanceJson(governanceReport);

      if (!governanceReport.approved) {
        result.risk = toRiskLevel(evaluation.bestPlan.riskClassification);
        result.ok = false;
        result.message = governanceReport.reason;
        break;
      }

      nlohmann::ordered_json childResults = nlohmann::ordered_json::array();
      const std::string contextTarget =
          !evaluation.bestPlan.strategy.proposedActions.empty()
              ? evaluation.bestPlan.strategy.proposedActions.front().target
              : evaluation.normalizedIntent.goal.target;
      if (!contextTarget.empty()) {
        Action contextAction;
        contextAction.type = ActionType::ContextExtraction;
        contextAction.id = "context:" + evaluation.bestPlan.strategy.name;
        contextAction.target = contextTarget;
        contextAction.branch = state.snapshot.branch.toString();
        contextAction.snapshotVersion = state.snapshot.version;
        Result childResult = executeActionLocked(contextAction, state);
        childResults.push_back({
            {"impacted_nodes", childResult.impactedNodes},
            {"message", childResult.message},
            {"normalized_paths", childResult.normalizedPaths},
            {"ok", childResult.ok},
            {"payload", childResult.payload},
            {"risk", toString(childResult.risk)},
            {"type", toString(contextAction.type)},
        });
        result.impactedNodes.insert(result.impactedNodes.end(),
                                    childResult.impactedNodes.begin(),
                                    childResult.impactedNodes.end());
        result.normalizedPaths.insert(result.normalizedPaths.end(),
                                      childResult.normalizedPaths.begin(),
                                      childResult.normalizedPaths.end());
      }

      bool intentLoopFailed = false;
      std::string intentLoopFailReason;
      for (const intent::Action& strategyAction :
           evaluation.bestPlan.strategy.proposedActions) {
        Action childAction = strategyActionToKernelAction(strategyAction, state);
        Result childResult = executeActionLocked(childAction, state);
        childResults.push_back({
            {"impacted_nodes", childResult.impactedNodes},
            {"message", childResult.message},
            {"normalized_paths", childResult.normalizedPaths},
            {"ok", childResult.ok},
            {"payload", childResult.payload},
            {"risk", toString(childResult.risk)},
            {"target", childAction.target},
            {"type", toString(childAction.type)},
        });
        result.impactedNodes.insert(result.impactedNodes.end(),
                                    childResult.impactedNodes.begin(),
                                    childResult.impactedNodes.end());
        result.normalizedPaths.insert(result.normalizedPaths.end(),
                                      childResult.normalizedPaths.begin(),
                                      childResult.normalizedPaths.end());

        // If a generative action failed, stop — do NOT fall through to next
        // (analysis/SimulateChange) action in the list.
        if (!childResult.ok && childAction.type == ActionType::ModelGenerate) {
          intentLoopFailed = true;
          intentLoopFailReason = childResult.message;
          break;
        }
      }

      result.payload["child_results"] = std::move(childResults);

      if (intentLoopFailed) {
        result.risk = RiskLevel::Medium;
        result.ok = false;
        result.message = intentLoopFailReason;
        break;
      }

      result.risk = toRiskLevel(evaluation.bestPlan.riskClassification);
      result.ok = true;
      result.message = "Intent plan executed deterministically.";
      break;
    }
  }

  sortOutputs(result);
  return result;
}

Result ExecutionKernel::execute(const Action& action,
                                const CognitiveState& state) {
  Result result;
  result.type = action.type;
  const std::string actionId = stableActionId(action);

  try {
    stateManager_.cognitiveMemory().recordIntentStart(
        actionId, state.snapshot, action.riskScore, action.confidenceScore,
        "execution_requested:" + toString(action.type));
  } catch (...) {
  }

  const auto recordOutcome = [&]() {
    try {
      stateManager_.cognitiveMemory().recordIntentExecution(
          actionId, state.snapshot, result.ok, result.rolledBack, result.message);
      stateManager_.cognitiveMemory().recordRiskEvaluation(
          "execution:" + actionId, state.snapshot, action.riskScore,
          result.ok ? 0.20 : 0.90, action.confidenceScore,
          result.message.empty() ? toString(action.type) : result.message);
      if (result.rolledBack) {
        stateManager_.cognitiveMemory().recordMergeOutcome(
            "rollback:" + actionId, state.snapshot, false, result.message);
      }
    } catch (...) {
    }
  };

  try {
    validateAction(action, state);
    CognitiveRuntime runtime(stateManager_);
    if (action.type == ActionType::ToolExecution &&
        runtime.toolRegistry().get_tool(action.toolName) == nullptr) {
      throw std::runtime_error(
          "Tool execution requested an unregistered Layer 16 tool.");
    }
    SnapshotPinGuard pinGuard = runtime.pin(state);

    std::lock_guard<std::mutex> queueLock(mutationQueueMutex_);
    const std::uint64_t queueOrder = ++queueCounter_;
    pinGuard.assertCurrent();

    result = executeActionLocked(action, state);
    result.queueOrder = queueOrder;
    updateToolFailureHistory(action, result);
  } catch (const std::exception& ex) {
    result.ok = false;
    result.type = action.type;
    result.message = ex.what();
  } catch (...) {
    result.ok = false;
    result.type = action.type;
    result.message = "Execution failed with an unknown error.";
  }

  sortOutputs(result);
  recordOutcome();
  // Cache last successful result so CognitiveRuntime can extract model output.
  if (result.ok) {
    lastResult_ = result;
    hasLastResult_ = true;
  }
  return result;
}

bool ExecutionKernel::validateSymbolWithUltra(const std::string& symbol) const {
  if (symbol.empty()) {
    return false;
  }

  const std::string command = "ultra ai_query " + quoteForShell(symbol);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const CommandProbe probe = runCapturedCommand(command);
    if (probe.exitCode == 0 && !hasUltraQueryError(probe.output)) {
      return true;
    }
    if (isUltraQueryTargetMissing(probe.output)) {
      return false;
    }

    core::Logger::warning(
        core::LogCategory::General,
        "Governance symbol validation retry " + std::to_string(attempt + 1) +
            " failed for '" + symbol + "'.");
  }

  const auto symbolsTablePath = resolveSymbolsTablePath();
  if (!symbolsTablePath.has_value()) {
    return false;
  }

  const bool fallbackFound = symbolsTableContains(*symbolsTablePath, symbol);
  if (fallbackFound) {
    core::Logger::warning(
        core::LogCategory::General,
        "Governance symbol validation used symbols.tbl fallback for '" + symbol +
            "'.");
  }
  return fallbackFound;
}
void ExecutionKernel::updateToolFailureHistory(
    const Action& action,
    const Result& result) {
  if (action.type != ActionType::ToolExecution || action.toolName.empty()) {
    return;
  }

  if (result.ok) {
    toolFailureCounts_[action.toolName] = 0U;
    return;
  }

  ++toolFailureCounts_[action.toolName];
}

bool ExecutionKernel::hasToolCognitionLayer() const noexcept {
  static const char* const kRequiredTools[] = {
      "query_symbol",
      "read_source",
      "impact_analysis",
      "get_context",
      "get_status",
  };

  for (const char* tool : kRequiredTools) {
    if (toolExecutor_.registry().get_tool(tool) == nullptr) {
      return false;
    }
  }
  return true;
}

bool ExecutionKernel::hasToolRouterLayer() const noexcept {
  return toolExecutor_.has_router();
}

}  // namespace ultra::runtime