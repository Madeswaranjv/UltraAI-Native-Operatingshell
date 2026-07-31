#include "ExecutionKernel.h"

#include "CognitiveRuntime.h"
#include "contract_enforcement.h"

#include "../ContextExtractor.h"
#include "../governance/GovernanceEngine.h"
#include "../impact_analyzer.h"
#include "../intent/IntentEvaluator.h"
#include "../../ai/orchestration/MultiModelOrchestrator.h"
#include "../../core/Logger.h"
#include "../../core/state_manager.h"
#include "../../diff/DiffEngine.h"
#include "../../engine/impact/ImpactPredictionEngine.h"
#include "../../cli/CommandOptions.h"
#include "../../adapters/AdapterCommandRunner.h"
#include "../CPUGovernor.h"

#include <algorithm>
#include <chrono>
#include <cctype>
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
#include <thread>
#include <utility>
#include <vector>
#include <iostream>
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

std::string astContextTargetForPath(const std::filesystem::path& projectRoot,
                                    const std::string& value) {
  const std::string normalized = normalizePathToken(value);
  if (normalized.empty()) {
    return ".";
  }

  std::filesystem::path raw(normalized);
  std::filesystem::path absolute =
      raw.is_absolute() ? raw.lexically_normal()
                        : (projectRoot / raw).lexically_normal();
  std::error_code ec;
  if (std::filesystem::is_regular_file(absolute, ec)) {
    const std::filesystem::path parent = absolute.parent_path();
    if (parent.empty()) {
      return ".";
    }
    const std::filesystem::path relative = parent.lexically_relative(projectRoot);
    if (!relative.empty()) {
      return relative.generic_string();
    }
    return parent.generic_string();
  }

  if (raw.has_extension()) {
    const std::filesystem::path parent = raw.parent_path();
    return parent.empty() ? "." : parent.generic_string();
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

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string providerEndpointForProject(const std::filesystem::path& projectRoot,
                                       const std::string& providerName) {
  const std::string normalizedProvider = lowerAscii(providerName);
  if (normalizedProvider.empty()) {
    return {};
  }

  const std::filesystem::path configPath = projectRoot / ".ultra" / "models.json";
  std::ifstream input(configPath);
  if (input.is_open()) {
    try {
      nlohmann::ordered_json parsed;
      input >> parsed;
      const auto providersIt = parsed.find("providers");
      if (providersIt != parsed.end() && providersIt->is_object()) {
        const auto providerIt = providersIt->find(normalizedProvider);
        if (providerIt != providersIt->end() && providerIt->is_object()) {
          if (providerIt->contains("endpoint") &&
              providerIt->at("endpoint").is_string()) {
            return providerIt->at("endpoint").get<std::string>();
          }
          if (providerIt->contains("base_url") &&
              providerIt->at("base_url").is_string()) {
            return providerIt->at("base_url").get<std::string>();
          }
        }
      }
    } catch (...) {
    }
  }

  if (normalizedProvider == "ollama") {
    return "http://localhost:11434";
  }
  return {};
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

std::string trimAscii(std::string value) {
  const auto notSpace = [](const unsigned char ch) {
    return !std::isspace(ch);
  };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
              value.end());
  return value;
}

std::string stripMarkdownCodeFence(const std::string& text) {
  std::string trimmed = trimAscii(text);
  if (trimmed.rfind("```", 0U) != 0U) {
    return trimmed;
  }

  const std::size_t bodyStart = trimmed.find('\n');
  const std::size_t fenceEnd = trimmed.rfind("```");
  if (bodyStart == std::string::npos || fenceEnd <= bodyStart) {
    return trimmed;
  }
  return trimAscii(trimmed.substr(bodyStart + 1U, fenceEnd - bodyStart - 1U));
}

std::string normalizeToolName(std::string value) {
  value = trimAscii(std::move(value));
  std::replace(value.begin(), value.end(), '-', '_');
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string jsonValueToString(const nlohmann::ordered_json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "true" : "false";
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<long long>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<unsigned long long>());
  }
  if (value.is_number_float()) {
    std::ostringstream stream;
    stream << value.get<double>();
    return stream.str();
  }
  if (value.is_null()) {
    return {};
  }
  return value.dump();
}

std::optional<nlohmann::ordered_json> parseJsonFromText(const std::string& text) {
  const std::string stripped = stripMarkdownCodeFence(text);
  if (stripped.empty()) {
    return std::nullopt;
  }

  const auto tryParse = [](const std::string& candidate)
      -> std::optional<nlohmann::ordered_json> {
    try {
      return nlohmann::ordered_json::parse(candidate);
    } catch (...) {
      return std::nullopt;
    }
  };

  if (std::optional<nlohmann::ordered_json> parsed = tryParse(stripped);
      parsed.has_value()) {
    return parsed;
  }

  const std::vector<std::pair<char, char>> delimiters = {
      {'{', '}'},
      {'[', ']'},
  };
  for (const auto& delimiter : delimiters) {
    const std::size_t start = stripped.find(delimiter.first);
    const std::size_t end = stripped.rfind(delimiter.second);
    if (start == std::string::npos || end == std::string::npos || end <= start) {
      continue;
    }
    if (std::optional<nlohmann::ordered_json> parsed =
            tryParse(stripped.substr(start, end - start + 1U));
        parsed.has_value()) {
      return parsed;
    }
  }

  return std::nullopt;
}

nlohmann::ordered_json toolArgumentsFromJsonObject(
    const nlohmann::ordered_json& object) {
  if (!object.is_object()) {
    return nlohmann::ordered_json::object();
  }

  const auto parseArgumentNode = [](const nlohmann::ordered_json& value) {
    if (value.is_object()) {
      return value;
    }
    if (value.is_string()) {
      if (std::optional<nlohmann::ordered_json> parsed =
              parseJsonFromText(value.get<std::string>());
          parsed.has_value() && parsed->is_object()) {
        return *parsed;
      }
      nlohmann::ordered_json payload = nlohmann::ordered_json::object();
      payload["input"] = value.get<std::string>();
      return payload;
    }
    nlohmann::ordered_json payload = nlohmann::ordered_json::object();
    payload["input"] = value;
    return payload;
  };

  if (object.contains("function") && object.at("function").is_object()) {
    const auto& functionObject = object.at("function");
    if (functionObject.contains("arguments")) {
      return parseArgumentNode(functionObject.at("arguments"));
    }
    nlohmann::ordered_json payload = nlohmann::ordered_json::object();
    for (auto it = functionObject.begin(); it != functionObject.end(); ++it) {
      if (it.key() == "name") {
        continue;
      }
      payload[it.key()] = it.value();
    }
    return payload;
  }

  if (object.contains("arguments")) {
    return parseArgumentNode(object.at("arguments"));
  }
  if (object.contains("args")) {
    return parseArgumentNode(object.at("args"));
  }

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  static const std::set<std::string> kReservedKeys = {
      "tool",
      "name",
      "type",
      "function",
  };
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (kReservedKeys.find(it.key()) != kReservedKeys.end()) {
      continue;
    }
    payload[it.key()] = it.value();
  }
  return payload;
}

std::optional<ai::model::ToolCall> toolCallFromJsonObject(
    const nlohmann::ordered_json& object) {
  if (!object.is_object()) {
    return std::nullopt;
  }

  ai::model::ToolCall toolCall;
  if (object.contains("tool") && object.at("tool").is_string()) {
    toolCall.name = normalizeToolName(object.at("tool").get<std::string>());
  }
  if (toolCall.name.empty() && object.contains("name") && object.at("name").is_string()) {
    toolCall.name = normalizeToolName(object.at("name").get<std::string>());
  }
  if (toolCall.name.empty() && object.contains("function") &&
      object.at("function").is_object()) {
    const auto& functionObject = object.at("function");
    if (functionObject.contains("name") && functionObject.at("name").is_string()) {
      toolCall.name = normalizeToolName(functionObject.at("name").get<std::string>());
    }
  }
  if (toolCall.name.empty()) {
    return std::nullopt;
  }

  toolCall.arguments = toolArgumentsFromJsonObject(object);
  return toolCall;
}

void appendToolCallsFromJson(const nlohmann::ordered_json& value,
                             std::vector<ai::model::ToolCall>& toolCalls) {
  if (value.is_array()) {
    for (const auto& entry : value) {
      appendToolCallsFromJson(entry, toolCalls);
    }
    return;
  }

  if (!value.is_object()) {
    return;
  }

  if (value.contains("tool_calls")) {
    appendToolCallsFromJson(value.at("tool_calls"), toolCalls);
  }
  if (value.contains("tool_call")) {
    appendToolCallsFromJson(value.at("tool_call"), toolCalls);
  }
  if (value.contains("payload")) {
    appendToolCallsFromJson(value.at("payload"), toolCalls);
  }
  if (value.contains("response")) {
    appendToolCallsFromJson(value.at("response"), toolCalls);
  }

  if (std::optional<ai::model::ToolCall> toolCall = toolCallFromJsonObject(value);
      toolCall.has_value()) {
    toolCalls.push_back(std::move(*toolCall));
  }
}

std::vector<ai::model::ToolCall> extractToolCallsFromText(const std::string& text) {
  std::vector<ai::model::ToolCall> toolCalls;
  if (std::optional<nlohmann::ordered_json> parsed = parseJsonFromText(text);
      parsed.has_value()) {
    appendToolCallsFromJson(*parsed, toolCalls);
  }
  return toolCalls;
}

std::map<std::string, std::string> orderedJsonToStringMap(
    const nlohmann::ordered_json& value) {
  std::map<std::string, std::string> result;
  if (!value.is_object()) {
    return result;
  }

  for (auto it = value.begin(); it != value.end(); ++it) {
    result[it.key()] = jsonValueToString(it.value());
  }
  return result;
}

std::optional<nlohmann::ordered_json> parseToolOutputJson(
    const std::string& output) {
  std::string candidate = trimAscii(output);
  if (candidate.rfind("ERROR:", 0U) == 0U) {
    candidate = trimAscii(candidate.substr(6U));
  } else if (candidate.rfind("[ERROR]", 0U) == 0U) {
    candidate = trimAscii(candidate.substr(7U));
  }
  return parseJsonFromText(candidate);
}

std::string summarizeExecutedTools(
    const std::vector<ai::model::ToolCall>& toolCalls) {
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (const ai::model::ToolCall& toolCall : toolCalls) {
    if (toolCall.name.empty() || !seen.insert(toolCall.name).second) {
      continue;
    }
    names.push_back(toolCall.name);
  }
  if (names.empty()) {
    return "Executed tool calls.";
  }

  std::ostringstream stream;
  stream << "Executed tool calls: ";
  for (std::size_t index = 0U; index < names.size(); ++index) {
    if (index > 0U) {
      stream << ", ";
    }
    stream << names[index];
  }
  stream << '.';
  return stream.str();
}

std::string summarizeToolCallNames(
    const std::vector<ai::model::ToolCall>& toolCalls) {
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (const ai::model::ToolCall& toolCall : toolCalls) {
    if (toolCall.name.empty() || !seen.insert(toolCall.name).second) {
      continue;
    }
    names.push_back(toolCall.name);
  }
  if (names.empty()) {
    return "none";
  }

  std::ostringstream stream;
  for (std::size_t index = 0U; index < names.size(); ++index) {
    if (index > 0U) {
      stream << ", ";
    }
    stream << names[index];
  }
  return stream.str();
}

constexpr std::size_t kApplyPatchMaxRetries = 2U;
constexpr std::size_t kApplyPatchPreviewLimit = 768U;

nlohmann::ordered_json stringMapToJson(
    const std::map<std::string, std::string>& values) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  for (const auto& [key, value] : values) {
    payload[key] = value;
  }
  return payload;
}

std::string truncateForRetryPreview(std::string value,
                                    const std::size_t limitBytes) {
  if (value.size() <= limitBytes) {
    return value;
  }

  constexpr std::string_view kNotice = "\n...[truncated for retry context]";
  const std::size_t keep =
      limitBytes > kNotice.size() ? limitBytes - kNotice.size() : 0U;
  value.resize(keep);
  value += kNotice;
  return value;
}

std::string extractApplyPatchPreview(
    const std::map<std::string, std::string>& toolArgs) {
  const auto diffIt = toolArgs.find("diff");
  if (diffIt != toolArgs.end() && !diffIt->second.empty()) {
    return truncateForRetryPreview(diffIt->second, kApplyPatchPreviewLimit);
  }

  const auto changesIt = toolArgs.find("changes");
  if (changesIt != toolArgs.end() && !changesIt->second.empty()) {
    return truncateForRetryPreview(changesIt->second, kApplyPatchPreviewLimit);
  }

  return {};
}

bool allToolCallsUseApplyPatch(
    const std::vector<ai::model::ToolCall>& toolCalls) {
  return !toolCalls.empty() &&
         std::all_of(toolCalls.begin(),
                     toolCalls.end(),
                     [](const ai::model::ToolCall& toolCall) {
                       return normalizeToolName(toolCall.name) == "apply_patch";
                     });
}

bool applyPatchFailureDetected(const Result& result) {
  bool applied = result.applied;
  if (result.payload.contains("applied") &&
      result.payload.at("applied").is_boolean()) {
    applied = result.payload.at("applied").get<bool>();
  }

  bool fileVerified = false;
  if (result.payload.contains("file_verified") &&
      result.payload.at("file_verified").is_boolean()) {
    fileVerified = result.payload.at("file_verified").get<bool>();
  }

  const bool hasError =
      result.payload.contains("error") && result.payload.at("error").is_string() &&
      !trimAscii(result.payload.at("error").get<std::string>()).empty();

  return !result.ok || !applied || !fileVerified || hasError;
}

std::string describeApplyPatchFailure(const Result& result) {
  std::vector<std::string> reasons;

  if (result.payload.contains("error") && result.payload.at("error").is_string()) {
    const std::string error = trimAscii(result.payload.at("error").get<std::string>());
    if (!error.empty()) {
      reasons.push_back(error);
    }
  }

  bool applied = result.applied;
  if (result.payload.contains("applied") &&
      result.payload.at("applied").is_boolean()) {
    applied = result.payload.at("applied").get<bool>();
  }
  if (!applied) {
    reasons.push_back("applied=false");
  }

  bool fileVerified = false;
  if (result.payload.contains("file_verified") &&
      result.payload.at("file_verified").is_boolean()) {
    fileVerified = result.payload.at("file_verified").get<bool>();
  }
  if (!fileVerified) {
    reasons.push_back("file_verified=false");
  }

  if (!result.message.empty()) {
    reasons.push_back(result.message);
  }

  if (reasons.empty()) {
    reasons.push_back("apply_patch returned an unsuccessful result");
  }

  sortAndDedupe(reasons);
  return joinReasons(reasons);
}

nlohmann::ordered_json buildApplyPatchRetryRecord(
    const std::size_t retryAttempt,
    const std::string& failureReason,
    const std::map<std::string, std::string>& toolArgs,
    const Result& result) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["retry_attempt"] = retryAttempt;
  payload["tool"] = "apply_patch";
  payload["failure_reason"] = failureReason;
  payload["previous_args"] = stringMapToJson(toolArgs);
  payload["previous_result"] = {
      {"message", result.message},
      {"ok", result.ok},
      {"applied", result.applied},
  };

  if (result.payload.contains("file_verified") &&
      result.payload.at("file_verified").is_boolean()) {
    payload["previous_result"]["file_verified"] =
        result.payload.at("file_verified").get<bool>();
  }
  if (result.payload.contains("error") && result.payload.at("error").is_string()) {
    payload["previous_result"]["error"] =
        result.payload.at("error").get<std::string>();
  }

  const std::string preview = extractApplyPatchPreview(toolArgs);
  if (!preview.empty()) {
    payload["previous_patch_preview"] = preview;
  }

  return payload;
}

ai::model::ModelRequest buildApplyPatchRetryRequest(
    const ai::model::ModelRequest& request,
    const std::size_t retryAttempt,
    const std::string& failureReason,
    const std::map<std::string, std::string>& toolArgs,
    const Result& result) {
  ai::model::ModelRequest next = request;

  std::ostringstream prompt;
  prompt << request.prompt;
  prompt << "\n\nPrevious apply_patch attempt " << retryAttempt
         << " failed.\n";
  prompt << "Failure reason: " << failureReason << "\n";
  prompt << "Generate a corrected unified diff for apply_patch. "
            "Do not repeat the previous diff unchanged. "
            "Return only the corrected tool call.\n";

  const std::string preview = extractApplyPatchPreview(toolArgs);
  if (!preview.empty()) {
    prompt << "Previous diff preview:\n" << preview << "\n";
  }
  next.prompt = prompt.str();

  if (next.systemPrompt.find(
          "Do not repeat the previous diff unchanged.") == std::string::npos) {
    if (!next.systemPrompt.empty() && next.systemPrompt.back() != ' ') {
      next.systemPrompt.push_back(' ');
    }
    next.systemPrompt +=
        "When apply_patch fails, correct the diff using the failure context "
        "and do not repeat the previous diff unchanged.";
  }

  if (!next.contextPayload.is_object()) {
    next.contextPayload = nlohmann::ordered_json::object();
  }
  next.contextPayload["apply_patch_retry"] =
      buildApplyPatchRetryRecord(retryAttempt, failureReason, toolArgs, result);
  next.contextPayload["tool_required"] = "apply_patch";
  return next;
}

const std::map<std::string, std::vector<std::string>>& fallbackToolRequiredParams() {
  static const std::map<std::string, std::vector<std::string>> kToolInputs = {
      {"read_file", {"path"}},
      {"write_file", {"path", "content"}},
      {"append_file", {"path", "content"}},
      {"delete_file", {"path"}},
      {"list_dir", {}},
      {"search_files", {"pattern"}},
      {"simulate_intent", {"goal"}},
      {"apply_patch", {}},
      {"run_command", {"command"}},
  };
  return kToolInputs;
}

const std::map<std::string, std::vector<std::string>>& fallbackToolAllowedParams() {
  static const std::map<std::string, std::vector<std::string>> kToolInputs = {
      {"read_file", {"path"}},
      {"write_file", {"path", "content", "create_dirs"}},
      {"append_file", {"path", "content"}},
      {"delete_file", {"path"}},
      {"list_dir", {"path", "recursive"}},
      {"search_files", {"pattern", "path", "case_sensitive"}},
      {"simulate_intent", {"goal", "target", "budget", "depth", "threshold"}},
      {"apply_patch", {"diff", "changes", "file", "path", "project_path"}},
      {"run_command", {"command", "cwd", "timeout_ms"}},
  };
  return kToolInputs;
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
  (void)strategyAction;
  return "auto";
}

ai::model::ModelRequest buildModelRequestForStrategyAction(
    const intent::Action& strategyAction,
    const CognitiveState& state) {
  ai::model::ModelRequest request;
  const std::string defaultRole =
      taskTypeForStrategyAction(strategyAction) ==
              ai::orchestration::TaskType::Coding
          ? "coder"
          : "analyzer";
  request.systemPrompt =
      "You are UltraInfinity deterministic execution. Return only real "
      "filesystem tool calls. Do not simulate, explain, or describe edits.";

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
  prompt << "Use the native apply_patch tool only. Return either tool_calls or "
            "a single JSON object like "
            "{\"tool\":\"apply_patch\",\"changes\":\"<unified diff>\"}.\n";
  prompt << "The patch must be a unified diff that can be applied directly to "
            "real files. No prose.";
  request.prompt = prompt.str();

  request.temperature = 0.05;
  const std::size_t estimatedBudget =
      strategyAction.estimatedFilesChanged * 320U +
      strategyAction.estimatedDependencyDepth * 160U;
  request.maxTokens = std::clamp<std::size_t>(estimatedBudget, 384U, 1536U);
  request.toolsAvailable = {"apply_patch"};

  request.contextPayload = {
      {"action_kind", intent::toString(strategyAction.kind)},
      {"target", strategyAction.target},
      {"details", strategyAction.details},
      {"estimated_files_changed", strategyAction.estimatedFilesChanged},
      {"estimated_dependency_depth", strategyAction.estimatedDependencyDepth},
      {"branch", state.snapshot.branch.toString()},
      {"model_role", defaultRole},
      {"requested_role", defaultRole},
      {"snapshot_version", state.snapshot.version},
      {"stage", defaultRole},
      {"tool_required", "apply_patch"},
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
  context.availableModels = {selectProviderForAction(strategyAction)};
  context.modelRoleHint =
      context.taskType == ai::orchestration::TaskType::Coding ? "coder"
                                                              : "analyzer";
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

std::string canonicalRoleName(std::string value) {
  value = lowerAscii(trimAscii(std::move(value)));
  if (value.empty() || value == "auto") {
    return {};
  }
  if (value == "plan" || value == "planner" || value == "planning") {
    return "planner";
  }
  if (value == "verify" || value == "verifier" || value == "verification" ||
      value == "validate" || value == "validation") {
    return "verifier";
  }
  if (value == "code" || value == "coder" || value == "coding" ||
      value == "implement" || value == "implementation") {
    return "coder";
  }
  if (value == "analysis" || value == "analyzer" || value == "analyse" ||
      value == "analyze" || value == "understand" || value == "explain") {
    return "analyzer";
  }
  return {};
}

std::string defaultRoleForTaskType(
    const ai::orchestration::TaskType taskType) {
  switch (taskType) {
    case ai::orchestration::TaskType::Planning:
      return "planner";
    case ai::orchestration::TaskType::Coding:
      return "coder";
    case ai::orchestration::TaskType::Analysis:
      return "analyzer";
    case ai::orchestration::TaskType::Reasoning:
      return {};
  }
  return {};
}

std::string payloadStringValue(const nlohmann::ordered_json& payload,
                               const char* key) {
  if (!payload.is_object() || !payload.contains(key) ||
      !payload.at(key).is_string()) {
    return {};
  }
  return trimAscii(payload.at(key).get<std::string>());
}

std::vector<std::string> payloadStringArrayValue(
    const nlohmann::ordered_json& payload,
    const char* key) {
  std::vector<std::string> values;
  if (!payload.is_object() || !payload.contains(key) ||
      !payload.at(key).is_array()) {
    return values;
  }

  for (const auto& item : payload.at(key)) {
    if (!item.is_string()) {
      continue;
    }
    const std::string normalized = normalizePathToken(trimAscii(item.get<std::string>()));
    if (!normalized.empty()) {
      values.push_back(normalized);
    }
  }
  sortAndDedupe(values);
  return values;
}

bool payloadContainsKey(const nlohmann::ordered_json& payload, const char* key) {
  return payload.is_object() && payload.contains(key);
}

bool looksLikeSourceTarget(const std::string& value) {
  const std::string trimmed = trimAscii(value);
  if (trimmed.empty()) {
    return false;
  }
  if (trimmed.find('/') != std::string::npos ||
      trimmed.find('\\') != std::string::npos) {
    return true;
  }

  const std::string extension =
      lowerAscii(std::filesystem::path(trimmed).extension().string());
  return extension == ".c" || extension == ".cc" || extension == ".cpp" ||
         extension == ".cxx" || extension == ".h" || extension == ".hh" ||
         extension == ".hpp" || extension == ".ipp" || extension == ".inl" ||
         extension == ".py" || extension == ".js" || extension == ".ts" ||
         extension == ".tsx" || extension == ".json" || extension == ".cmake";
}

bool containsAnyKeyword(const std::string& text,
                        const std::initializer_list<std::string_view> keywords) {
  for (const std::string_view keyword : keywords) {
    if (text.find(keyword) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool hasToolAvailable(const ai::model::ModelRequest& request,
                      const std::string_view toolName) {
  return std::any_of(request.toolsAvailable.begin(),
                     request.toolsAvailable.end(),
                     [&](const std::string& tool) {
                       return lowerAscii(trimAscii(tool)) == toolName;
                     });
}

std::string buildRoutingSignalText(const Action& action,
                                   const ai::model::ModelRequest& request) {
  std::ostringstream stream;
  stream << action.id << '\n' << action.target << '\n' << request.prompt << '\n'
         << request.systemPrompt;

  if (!request.contextPayload.is_object()) {
    return lowerAscii(stream.str());
  }

  const auto& payload = request.contextPayload;
  const char* const keys[] = {
      "action_kind",    "coder_message", "details",        "error",
      "failure_reason", "failure_type",  "model_role",     "original_task",
      "requested_role", "stage",         "target",         "task_id",
      "tool_required",
  };
  for (const char* key : keys) {
    const std::string value = payloadStringValue(payload, key);
    if (!value.empty()) {
      stream << '\n' << value;
    }
  }

  if (payloadContainsKey(payload, "apply_patch_retry")) {
    stream << '\n' << payload.at("apply_patch_retry").dump();
  }
  if (payloadContainsKey(payload, "coder_payload")) {
    stream << '\n' << payload.at("coder_payload").dump();
  }
  if (payloadContainsKey(payload, "intent")) {
    stream << '\n' << payload.at("intent").dump();
  }
  if (payloadContainsKey(payload, "resolved_intent")) {
    stream << '\n' << payload.at("resolved_intent").dump();
  }

  return lowerAscii(stream.str());
}

bool hasSourceCodeSignal(const Action& action,
                         const ai::model::ModelRequest& request) {
  if (!request.contextPayload.is_object()) {
    return looksLikeSourceTarget(action.target) ||
           request.prompt.find("```") != std::string::npos;
  }

  const auto& payload = request.contextPayload;
  if (looksLikeSourceTarget(action.target) ||
      looksLikeSourceTarget(payloadStringValue(payload, "target")) ||
      looksLikeSourceTarget(payloadStringValue(payload, "source_file")) ||
      looksLikeSourceTarget(payloadStringValue(payload, "file")) ||
      payloadContainsKey(payload, "file_targets") ||
      payloadContainsKey(payload, "source_file") ||
      hasToolAvailable(request, "apply_patch")) {
    return true;
  }

  const std::string prompt = lowerAscii(request.prompt);
  return prompt.find("```") != std::string::npos ||
         prompt.find("#include") != std::string::npos ||
         prompt.find("class ") != std::string::npos ||
         prompt.find("struct ") != std::string::npos ||
         prompt.find("namespace ") != std::string::npos;
}

struct RoleRoutingDecision {
  std::string taskId;
  std::string stage;
  std::string requestedRole;
  std::string resolvedRole;
};

RoleRoutingDecision resolveModelRole(
    const Action& action,
    const ai::model::ModelRequest& request,
    const ai::orchestration::OrchestrationContext& context) {
  const nlohmann::ordered_json payload =
      request.contextPayload.is_object() ? request.contextPayload
                                         : nlohmann::ordered_json::object();
  const std::string requestedRole =
      canonicalRoleName(payloadStringValue(payload, "requested_role"));
  const std::string modelRoleHint = [&]() {
    const std::string payloadRole =
        canonicalRoleName(payloadStringValue(payload, "model_role"));
    if (!payloadRole.empty()) {
      return payloadRole;
    }
    return canonicalRoleName(context.modelRoleHint);
  }();
  const std::string stage = [&]() {
    const std::string payloadStage = trimAscii(payloadStringValue(payload, "stage"));
    if (!payloadStage.empty()) {
      return payloadStage;
    }
    return ai::orchestration::toString(context.taskType);
  }();
  const std::string taskId = [&]() {
    const std::string payloadTaskId =
        trimAscii(payloadStringValue(payload, "task_id"));
    if (!payloadTaskId.empty()) {
      return payloadTaskId;
    }
    return action.id;
  }();
  const std::string stageRole = canonicalRoleName(stage);

  RoleRoutingDecision decision;
  decision.taskId = taskId;
  decision.stage = stage;
  decision.requestedRole = requestedRole;

  const bool repairTaskLocked =
      payloadContainsKey(payload, "repair_task") &&
      payload.at("repair_task").is_boolean() &&
      payload.at("repair_task").get<bool>();
  if (repairTaskLocked) {
    if (!requestedRole.empty()) {
      decision.resolvedRole = requestedRole;
      return decision;
    }
    if (!modelRoleHint.empty()) {
      decision.resolvedRole = modelRoleHint;
      return decision;
    }
  }

  if (stageRole == "planner" || stageRole == "verifier") {
    decision.resolvedRole = stageRole;
    return decision;
  }

  struct RoleScores {
    int planner{0};
    int analyzer{0};
    int coder{0};
    int verifier{0};
  } scores;
  const auto addScore = [&](const std::string& role, const int value) {
    if (role == "planner") {
      scores.planner += value;
    } else if (role == "analyzer") {
      scores.analyzer += value;
    } else if (role == "coder") {
      scores.coder += value;
    } else if (role == "verifier") {
      scores.verifier += value;
    }
  };
  const auto scoreForRole = [&](const std::string& role) {
    if (role == "planner") {
      return scores.planner;
    }
    if (role == "analyzer") {
      return scores.analyzer;
    }
    if (role == "coder") {
      return scores.coder;
    }
    if (role == "verifier") {
      return scores.verifier;
    }
    return 0;
  };

  if (!stageRole.empty()) {
    addScore(stageRole, 2);
  }
  if (!modelRoleHint.empty()) {
    addScore(modelRoleHint, 1);
  }

  const std::string signalText = buildRoutingSignalText(action, request);
  const bool patchGenerationRequested =
      lowerAscii(payloadStringValue(payload, "tool_required")) == "apply_patch" ||
      hasToolAvailable(request, "apply_patch");
  const bool patchRetrySignal = payloadContainsKey(payload, "apply_patch_retry");
  const bool compileFailureSignal =
      containsAnyKeyword(signalText,
                         {"compile error", "compiler error", "build failed",
                          "build error", "compilation failed",
                          "failed to compile", "linker error", "link error",
                          "undefined reference", "undefined symbol",
                          "syntax error"});
  const bool logicFailureSignal =
      containsAnyKeyword(signalText,
                         {"logic failure", "logic bug", "wrong result",
                          "incorrect result", "unexpected result", "mismatch",
                          "regression", "root cause", "assertion failed"});
  const bool sourceCodeSignal = hasSourceCodeSignal(action, request);

  if (patchGenerationRequested) {
    scores.coder += 6;
  }
  if (patchRetrySignal) {
    scores.coder += 7;
  }
  if (compileFailureSignal) {
    scores.coder += 5;
  }
  if (logicFailureSignal) {
    scores.analyzer += patchGenerationRequested ? 2 : 5;
  }
  if (sourceCodeSignal) {
    scores.coder += 2;
    scores.analyzer += 1;
  }

  if (containsAnyKeyword(signalText,
                         {"plan ", "planner", "planning", "strategy",
                          "roadmap", "step-by-step", "outline", "design"})) {
    scores.planner += 4;
  }
  if (containsAnyKeyword(signalText,
                         {"verify", "verification", "verifier", "validate",
                          "validation", "confirm", "pass/fail"})) {
    scores.verifier += 4;
  }
  if (containsAnyKeyword(signalText,
                         {"modify", "write", "implement", "fix", "patch",
                          "edit", "refactor", "rename", "add ", "remove",
                          "update", "change "})) {
    scores.coder += 4;
  }
  if (containsAnyKeyword(signalText,
                         {"analyse", "analyze", "analysis", "explain",
                          "understand", "investigate", "inspect",
                          "summarize", "summary", "why "})) {
    scores.analyzer += 4;
  }

  const std::string loweredTaskId = lowerAscii(taskId);
  if (containsAnyKeyword(loweredTaskId, {"planner", "plan"})) {
    scores.planner += 3;
  }
  if (containsAnyKeyword(loweredTaskId, {"verifier", "verify"})) {
    scores.verifier += 3;
  }
  if (containsAnyKeyword(loweredTaskId, {"coder", "patch", "edit", "refactor"})) {
    scores.coder += 3;
  }
  if (containsAnyKeyword(loweredTaskId,
                         {"analyzer", "analysis", "analyse", "analyze",
                          "explain"})) {
    scores.analyzer += 3;
  }

  const int maxScore = std::max({scores.planner, scores.analyzer, scores.coder,
                                 scores.verifier});
  if (maxScore > 0) {
    if (!stageRole.empty() && scoreForRole(stageRole) == maxScore) {
      decision.resolvedRole = stageRole;
    } else if (!modelRoleHint.empty() &&
               scoreForRole(modelRoleHint) == maxScore) {
      decision.resolvedRole = modelRoleHint;
    } else if (scores.verifier == maxScore) {
      decision.resolvedRole = "verifier";
    } else if (scores.coder == maxScore) {
      decision.resolvedRole = "coder";
    } else if (scores.analyzer == maxScore) {
      decision.resolvedRole = "analyzer";
    } else if (scores.planner == maxScore) {
      decision.resolvedRole = "planner";
    }
  }

  if (decision.resolvedRole.empty()) {
    decision.resolvedRole = requestedRole;
  }
  if (decision.resolvedRole.empty()) {
    decision.resolvedRole = modelRoleHint;
  }
  if (decision.resolvedRole.empty()) {
    decision.resolvedRole = defaultRoleForTaskType(context.taskType);
  }
  if (decision.resolvedRole.empty()) {
    decision.resolvedRole = "analyzer";
  }
  return decision;
}

ai::orchestration::OrchestrationContext withResolvedRole(
    ai::orchestration::OrchestrationContext context,
    const std::string& resolvedRole) {
  if (!resolvedRole.empty()) {
    context.modelRoleHint = resolvedRole;
  }
  return context;
}

bool endsWithRepairSuffix(const std::string_view value) {
  constexpr std::string_view kRepairSuffix = "_repair";
  return value.size() >= kRepairSuffix.size() &&
         value.substr(value.size() - kRepairSuffix.size()) == kRepairSuffix;
}

std::string repairTaskIdFor(const std::string& originalTaskId) {
  if (originalTaskId.empty() || endsWithRepairSuffix(originalTaskId)) {
    return originalTaskId;
  }
  return originalTaskId + "_repair";
}

std::mutex& failureIntelligenceMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, FailureIntelligence>& failureIntelligenceStore() {
  static std::map<std::string, FailureIntelligence> store;
  return store;
}

void appendFailureText(std::string& block, const std::string& value) {
  const std::string trimmed = trimAscii(value);
  if (trimmed.empty()) {
    return;
  }
  if (!block.empty()) {
    block += '\n';
  }
  block += trimmed;
}

void appendFailureJsonString(std::string& block,
                             const nlohmann::ordered_json& payload,
                             const char* key) {
  const std::string value = payloadStringValue(payload, key);
  if (!value.empty()) {
    appendFailureText(block, value);
  }
}

void appendFailureToolResultText(std::string& block,
                                 const nlohmann::ordered_json& value) {
  if (!value.is_object()) {
    return;
  }

  appendFailureJsonString(block, value, "message");
  appendFailureJsonString(block, value, "text_output");
  appendFailureJsonString(block, value, "tool");
  if (value.contains("payload") && value.at("payload").is_object()) {
    const auto& payload = value.at("payload");
    appendFailureJsonString(block, payload, "error");
    appendFailureJsonString(block, payload, "message");
    appendFailureJsonString(block, payload, "output");
  }
}

std::string extractPreviousOutput(const Result& result) {
  if (!trimAscii(result.text_output).empty()) {
    return trimAscii(result.text_output);
  }
  if (!result.payload.is_object()) {
    return {};
  }
  if (const std::string modelOutput =
          payloadStringValue(result.payload, "model_text_output");
      !modelOutput.empty()) {
    return modelOutput;
  }
  if (result.payload.contains("response") && result.payload.at("response").is_object()) {
    if (const std::string responseOutput =
            payloadStringValue(result.payload.at("response"), "text_output");
        !responseOutput.empty()) {
      return responseOutput;
    }
  }
  return {};
}

std::string collectFailureText(const Result& result) {
  std::string block;
  appendFailureText(block, result.message);
  appendFailureText(block, result.text_output);
  appendFailureText(block, extractPreviousOutput(result));

  if (!result.payload.is_object()) {
    return block;
  }

  appendFailureJsonString(block, result.payload, "error");
  appendFailureJsonString(block, result.payload, "output");
  appendFailureJsonString(block, result.payload, "tool");
  appendFailureJsonString(block, result.payload, "tool_execution_summary");
  appendFailureJsonString(block, result.payload, "model_text_output");

  if (result.payload.contains("tool_execution") &&
      result.payload.at("tool_execution").is_object()) {
    const auto& toolExecution = result.payload.at("tool_execution");
    appendFailureJsonString(block, toolExecution, "tool");
    appendFailureJsonString(block, toolExecution, "error");
    appendFailureJsonString(block, toolExecution, "output");
  }

  if (result.payload.contains("response") && result.payload.at("response").is_object()) {
    const auto& response = result.payload.at("response");
    appendFailureJsonString(block, response, "error_message");
    appendFailureJsonString(block, response, "finish_reason");
    appendFailureJsonString(block, response, "text_output");
  }

  if (result.payload.contains("tool_results") &&
      result.payload.at("tool_results").is_array()) {
    for (const auto& toolResult : result.payload.at("tool_results")) {
      appendFailureToolResultText(block, toolResult);
    }
  }

  return block;
}

bool hasNonZeroExitCode(const nlohmann::ordered_json& value) {
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      const std::string key = lowerAscii(it.key());
      if ((key.find("exit_code") != std::string::npos || key == "returncode" ||
           key == "status_code") &&
          it.value().is_number()) {
        const double exitCode = it.value().get<double>();
        if (exitCode != 0.0) {
          return true;
        }
      }
      if (hasNonZeroExitCode(it.value())) {
        return true;
      }
    }
    return false;
  }

  if (!value.is_array()) {
    return false;
  }

  for (const auto& item : value) {
    if (hasNonZeroExitCode(item)) {
      return true;
    }
  }
  return false;
}

bool hasCompileFailureSignal(const Result& result, const std::string& loweredText) {
  if (containsAnyKeyword(loweredText,
                         {"compile error", "compiler error", "build failed",
                          "build error", "compilation failed",
                          "failed to compile", "linker error", "link error",
                          "undefined reference", "undefined symbol",
                          "syntax error", "fatal error", "ninja: build stopped",
                          "ld returned"})) {
    return true;
  }

  if (!result.payload.is_object()) {
    return false;
  }

  if (result.payload.contains("output_json") && result.payload.at("output_json").is_object()) {
    const auto& outputJson = result.payload.at("output_json");
    if ((outputJson.contains("build_exit_code") &&
         outputJson.at("build_exit_code").is_number() &&
         outputJson.at("build_exit_code").get<double>() != 0.0) ||
        (outputJson.contains("compile_exit_code") &&
         outputJson.at("compile_exit_code").is_number() &&
         outputJson.at("compile_exit_code").get<double>() != 0.0)) {
      return true;
    }
  }

  return hasNonZeroExitCode(result.payload) &&
         containsAnyKeyword(loweredText,
                            {"build", "compile", "compiler", "link", "cmake",
                             "ninja", "make", "msbuild"});
}

bool hasVerificationFailureSignal(const Result& result,
                                  const std::string& loweredText) {
  if (containsAnyKeyword(loweredText,
                         {"test fail", "tests fail", "failed tests",
                          "failing test", "ctest", "ai_verify",
                          "verification failed", "verify failed",
                          "verification mismatch", "assertion failed"})) {
    return true;
  }

  if (!result.payload.is_object()) {
    return false;
  }

  if (result.payload.contains("output_json") && result.payload.at("output_json").is_object()) {
    const auto& outputJson = result.payload.at("output_json");
    if ((outputJson.contains("test_exit_code") &&
         outputJson.at("test_exit_code").is_number() &&
         outputJson.at("test_exit_code").get<double>() != 0.0) ||
        (outputJson.contains("verify_exit_code") &&
         outputJson.at("verify_exit_code").is_number() &&
         outputJson.at("verify_exit_code").get<double>() != 0.0)) {
      return true;
    }
  }

  return false;
}

bool hasToolFailureSignal(const Result& result, const std::string& loweredText) {
  if (loweredText.find("no executable tool calls") != std::string::npos) {
    return false;
  }

  if (result.type == ActionType::ToolExecution) {
    return true;
  }

  if (containsAnyKeyword(loweredText,
                         {"apply_patch", "tool execution", "tool router",
                          "not registered", "exit code"})) {
    return true;
  }

  if (!result.payload.is_object()) {
    return false;
  }

  return payloadContainsKey(result.payload, "tool") ||
         payloadContainsKey(result.payload, "tool_execution") ||
         payloadContainsKey(result.payload, "tool_results") ||
         hasNonZeroExitCode(result.payload);
}

bool hasLogicFailureSignal(const Result& result, const std::string& loweredText) {
  if (containsAnyKeyword(loweredText,
                         {"logic failure", "logic bug", "wrong result",
                          "incorrect result", "unexpected result", "mismatch",
                          "contradict", "contradiction", "regression",
                          "inconsistent", "failed expected", "empty output",
                          "no executable tool calls"})) {
    return true;
  }

  if (result.type != ActionType::ModelGenerate) {
    return false;
  }

  const bool hasAnyToolResults =
      result.payload.is_object() && payloadContainsKey(result.payload, "tool_results");
  return trimAscii(extractPreviousOutput(result)).empty() && !hasAnyToolResults;
}

std::string actionSourceFileHint(const Action& action) {
  if (!action.modelRequest.has_value() ||
      !action.modelRequest->contextPayload.is_object()) {
    return looksLikeSourceTarget(action.target) ? action.target : std::string{};
  }

  const auto& payload = action.modelRequest->contextPayload;
  if (const std::string sourceFile = payloadStringValue(payload, "source_file");
      !sourceFile.empty()) {
    return sourceFile;
  }
  if (const std::string file = payloadStringValue(payload, "file"); !file.empty()) {
    return file;
  }
  if (const std::vector<std::string> selectedFiles =
          payloadStringArrayValue(payload, "selected_files");
      !selectedFiles.empty()) {
    return selectedFiles.front();
  }
  if (const std::vector<std::string> fileTargets =
          payloadStringArrayValue(payload, "file_targets");
      !fileTargets.empty()) {
    return fileTargets.front();
  }
  if (looksLikeSourceTarget(action.target)) {
    return action.target;
  }
  return {};
}

std::string actionOriginalTaskId(const Action& action) {
  if (action.modelRequest.has_value() &&
      action.modelRequest->contextPayload.is_object()) {
    if (const std::string originalTask =
            payloadStringValue(action.modelRequest->contextPayload, "original_task");
        !originalTask.empty()) {
      return originalTask;
    }
  }
  return action.id;
}

void clearFailureIntelligence(const std::string& taskId) {
  if (taskId.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(failureIntelligenceMutex());
  failureIntelligenceStore().erase(taskId);
}

void rememberFailureIntelligence(const FailureIntelligence& intelligence) {
  if (intelligence.taskId.empty()) {
    return;
  }

  std::lock_guard<std::mutex> lock(failureIntelligenceMutex());
  failureIntelligenceStore()[intelligence.taskId] = intelligence;
}

void updateFailureIntelligenceForAction(const Action& action, Result& result) {
  clearFailureIntelligence(action.id);
  result.failureType = ExecutionKernel::classifyFailure(result);
  result.repairRole = ExecutionKernel::routeFailureRole(result.failureType);

  if (result.ok && !result.rolledBack) {
    return;
  }

  if (result.failureType == FailureType::NONE) {
    return;
  }

  const std::string originalTaskId = actionOriginalTaskId(action);
  FailureIntelligence intelligence;
  intelligence.type = result.failureType;
  intelligence.taskId = action.id;
  intelligence.originalTaskId = originalTaskId;
  intelligence.repairTaskId = repairTaskIdFor(originalTaskId);
  intelligence.target = action.target;
  intelligence.sourceFile = actionSourceFileHint(action);
  intelligence.routedRole = result.repairRole;
  intelligence.previousOutput =
      truncateForRetryPreview(extractPreviousOutput(result), 1200U);
  intelligence.errorLogs =
      truncateForRetryPreview(collectFailureText(result), 2400U);
  intelligence.repairContext = {
      {"context",
       {{"error_logs", intelligence.errorLogs},
        {"previous_output", intelligence.previousOutput}}},
      {"failure_type", toString(intelligence.type)},
      {"original_task", intelligence.originalTaskId},
      {"target", intelligence.target},
      {"task_id", intelligence.repairTaskId},
      {"type", "repair"},
  };
  if (!intelligence.sourceFile.empty()) {
    intelligence.repairContext["source_file"] = intelligence.sourceFile;
  }
  rememberFailureIntelligence(intelligence);

  if (!result.payload.is_object()) {
    result.payload = nlohmann::ordered_json::object();
  }
  result.payload["failure"] = {
      {"original_task", intelligence.originalTaskId},
      {"repair_task", intelligence.repairTaskId},
      {"routed_role", intelligence.routedRole},
      {"target", intelligence.target},
      {"type", toString(intelligence.type)},
  };
  if (!intelligence.sourceFile.empty()) {
    result.payload["failure"]["source_file"] = intelligence.sourceFile;
  }
  result.payload["repair_context"] = intelligence.repairContext;

  std::cout << "[FAILURE] task=" << intelligence.originalTaskId
            << " type=" << toString(intelligence.type) << std::endl;
  std::cout << "[REPAIR] routing_to=" << intelligence.routedRole << std::endl;
}

}  // namespace

const char* toString(const FailureType type) noexcept {
  switch (type) {
    case FailureType::NONE:
      return "NONE";
    case FailureType::COMPILE_ERROR:
      return "COMPILE_ERROR";
    case FailureType::LOGIC_ERROR:
      return "LOGIC_ERROR";
    case FailureType::TEST_FAIL:
      return "TEST_FAIL";
    case FailureType::TOOL_ERROR:
      return "TOOL_ERROR";
  }
  return "NONE";
}

ExecutionKernel::ExecutionKernel(
    core::StateManager& stateManager,
    std::shared_ptr<ai::orchestration::IMultiModelOrchestrator> modelOrchestrator)
    : stateManager_(stateManager),
      modelOrchestrator_(modelOrchestrator != nullptr
                             ? std::move(modelOrchestrator)
                             : ai::orchestration::MultiModelOrchestrator::
                                   createDefault(stateManager.projectRoot())) {}

FailureType ExecutionKernel::classifyFailure(const ExecutionResult& result) {
  if (result.ok && !result.rolledBack) {
    return FailureType::NONE;
  }

  const std::string loweredText = lowerAscii(collectFailureText(result));
  if (hasCompileFailureSignal(result, loweredText)) {
    return FailureType::COMPILE_ERROR;
  }
  if (hasVerificationFailureSignal(result, loweredText)) {
    return FailureType::TEST_FAIL;
  }
  if (hasToolFailureSignal(result, loweredText)) {
    return FailureType::TOOL_ERROR;
  }
  if (hasLogicFailureSignal(result, loweredText)) {
    return FailureType::LOGIC_ERROR;
  }
  return FailureType::NONE;
}

std::string ExecutionKernel::routeFailureRole(const FailureType failureType) {
  switch (failureType) {
    case FailureType::COMPILE_ERROR:
      return "coder";
    case FailureType::LOGIC_ERROR:
      return "analyzer";
    case FailureType::TEST_FAIL:
      return "verifier";
    case FailureType::TOOL_ERROR:
      return "coder";
    case FailureType::NONE:
      return {};
  }
  return {};
}

std::optional<FailureIntelligence> ExecutionKernel::failureIntelligenceForTask(
    const std::string& taskId) {
  std::lock_guard<std::mutex> lock(failureIntelligenceMutex());
  const auto it = failureIntelligenceStore().find(taskId);
  if (it == failureIntelligenceStore().end()) {
    return std::nullopt;
  }
  return it->second;
}

Action ExecutionKernel::buildActionFromStrategy(
    const intent::Action& strategyAction,
    const CognitiveState& state) {
  return strategyActionToKernelAction(strategyAction, state);
}

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

  const std::string normalizedTool = normalizeToolName(tool);
  std::vector<std::string> reasons;
  std::size_t unknownParameterCount = 0U;

  const cognitive::tools::ToolDefinition* definition =
      toolExecutor_.registry().get_tool(normalizedTool);
  std::vector<std::string> requiredArgs;
  std::vector<std::string> allowedArgs;
  if (definition != nullptr) {
    requiredArgs = definition->input_params;
    if (const auto fallbackAllowed =
            fallbackToolAllowedParams().find(normalizedTool);
        fallbackAllowed != fallbackToolAllowedParams().end()) {
      allowedArgs = fallbackAllowed->second;
    } else {
      allowedArgs = definition->input_params;
    }
    if (requiredArgs.empty()) {
      if (const auto fallbackRequired =
              fallbackToolRequiredParams().find(normalizedTool);
          fallbackRequired != fallbackToolRequiredParams().end()) {
        requiredArgs = fallbackRequired->second;
      }
    }
  } else {
    const auto fallbackAllowed = fallbackToolAllowedParams().find(normalizedTool);
    if (fallbackAllowed == fallbackToolAllowedParams().end()) {
      decision.allowed = false;
      decision.risk = RiskLevel::Critical;
      decision.confidence = 0.0F;
      decision.reason = "Unknown tool: " + normalizedTool;
      core::Logger::warning(core::LogCategory::General,
                            "Governance blocked unknown tool '" +
                                normalizedTool + "'.");
      return decision;
    }

    allowedArgs = fallbackAllowed->second;
    if (const auto fallbackRequired =
            fallbackToolRequiredParams().find(normalizedTool);
        fallbackRequired != fallbackToolRequiredParams().end()) {
      requiredArgs = fallbackRequired->second;
    }
  }

  if (normalizedTool == "query_symbol" || normalizedTool == "read_source" ||
      normalizedTool == "read_file" || normalizedTool == "list_dir" ||
      normalizedTool == "search_files") {
    decision.risk = RiskLevel::Low;
  } else if (normalizedTool == "impact_analysis" ||
             normalizedTool == "simulate_intent" ||
             normalizedTool == "get_context" ||
             normalizedTool == "get_status") {
    decision.risk = RiskLevel::Medium;
  } else if (normalizedTool == "write_file" ||
             normalizedTool == "append_file" ||
             normalizedTool == "delete_file" ||
             normalizedTool == "run_command" ||
             normalizedTool == "apply_patch") {
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

  if (normalizedTool == "apply_patch") {
    const bool hasPatchPayload =
        (args.find("diff") != args.end() && !args.at("diff").empty()) ||
        (args.find("changes") != args.end() && !args.at("changes").empty());
    if (!hasPatchPayload) {
      decision.risk = maxRisk(decision.risk, RiskLevel::Critical);
      decision.confidence -= 0.45F;
      reasons.push_back("apply_patch requires diff or changes payload");
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
        std::find(allowedArgs.begin(), allowedArgs.end(), key) != allowedArgs.end();
    if (!known) {
      ++unknownParameterCount;
      decision.risk = maxRisk(decision.risk, RiskLevel::High);
      decision.confidence -= 0.15F;
      reasons.push_back("Unknown parameter: " + key);
    }
  }

  if (normalizedTool == "query_symbol" || normalizedTool == "impact_analysis") {
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
      std::max(toolExecutor_.consecutive_failures(normalizedTool),
               toolFailureCounts_[normalizedTool]);
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
      "Governance decision tool='" + normalizedTool + "' risk=" +
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
  if (action.type != ActionType::IntentEvaluation) {
    Result result;
    result.type = action.type;
    result.risk = RiskLevel::Critical;
    result.ok = false;
    result.message =
        "ExecutionKernel::executeIntent refuses to execute non-intent actions.";
    return result;
  }
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
      result.ok = false;
      result.risk = RiskLevel::Critical;
      result.payload = {
          {"target", action.target},
          {"disabled", true},
      };
      result.message =
          "SimulateChange is disabled; deterministic execution requires real tool calls.";
      break;
    }

    case ActionType::ModelGenerate: {
      const ai::orchestration::OrchestrationContext baseOrchestrationContext =
          buildOrchestrationContext(action);

      std::cerr << "[ULTRA-DEBUG] ModelGenerate entered."
                << " provider=" << action.modelProvider
                << " prompt_len=" << (action.modelRequest.has_value()
                                          ? action.modelRequest->prompt.size()
                                          : 0U)
                << " orchestrator="
                << (modelOrchestrator_ != nullptr ? "set" : "NULL")
                << "\n";

      lastModelTextOutput_.clear();
      lastSelectedProvider_.clear();
      lastProviderEndpoint_.clear();
      ai::model::ModelRequest currentRequest = *action.modelRequest;
      if (!currentRequest.contextPayload.is_object()) {
        currentRequest.contextPayload = nlohmann::ordered_json::object();
      }
      if (!action.id.empty()) {
        currentRequest.contextPayload["task_id"] = action.id;
      }
      if (!action.target.empty() &&
          (!currentRequest.contextPayload.contains("target") ||
           !currentRequest.contextPayload.at("target").is_string() ||
           currentRequest.contextPayload.at("target").get<std::string>().empty())) {
        currentRequest.contextPayload["target"] = action.target;
      }
      if (!currentRequest.contextPayload.contains("stage")) {
        currentRequest.contextPayload["stage"] =
            baseOrchestrationContext.taskType ==
                    ai::orchestration::TaskType::Coding
                ? "coder"
                : ai::orchestration::toString(baseOrchestrationContext.taskType);
      }
      nlohmann::ordered_json applyPatchRetryHistory =
          nlohmann::ordered_json::array();
      std::size_t applyPatchRetryCount = 0U;
      bool applyPatchRetryExhausted = false;
      RoleRoutingDecision routingDecision =
          resolveModelRole(action, currentRequest, baseOrchestrationContext);
      ai::orchestration::OrchestrationContext routedContext =
          withResolvedRole(baseOrchestrationContext, routingDecision.resolvedRole);
      if (!routingDecision.resolvedRole.empty()) {
        currentRequest.contextPayload["model_role"] = routingDecision.resolvedRole;
      }
      if (modelOrchestrator_ == nullptr) {
        ai::model::ModelResponse response;
        response.ok = false;
        response.errorCode = ai::model::ModelErrorCode::ProviderUnavailable;
        response.errorMessage = "Model orchestrator is unavailable.";
        result.payload = buildModelExecutionJson(routedContext,
                                                 action.modelProvider,
                                                 currentRequest, response);
        result.normalizedPaths = collectNormalizedPaths(result.payload);
        result.risk = RiskLevel::Medium;
        result.ok = false;
        result.message = response.errorMessage;
        break;
      }

      for (;;) {
        result.payload = nlohmann::ordered_json::object();
        result.impactedNodes.clear();
        result.normalizedPaths.clear();
        result.applied = false;
        result.text_output.clear();
        result.message.clear();
        result.ok = false;
        result.risk = RiskLevel::Low;

        routingDecision =
            resolveModelRole(action, currentRequest, baseOrchestrationContext);
        routedContext =
            withResolvedRole(baseOrchestrationContext, routingDecision.resolvedRole);
        if (!routingDecision.resolvedRole.empty()) {
          currentRequest.contextPayload["model_role"] =
              routingDecision.resolvedRole;
        }
        std::cout << "[ROUTER] task="
                  << (routingDecision.taskId.empty() ? "unknown"
                                                     : routingDecision.taskId)
                  << " stage="
                  << (routingDecision.stage.empty() ? "unknown"
                                                    : routingDecision.stage)
                  << " requested="
                  << (routingDecision.requestedRole.empty()
                          ? "auto"
                          : routingDecision.requestedRole)
                  << " resolved=" << routingDecision.resolvedRole << std::endl;
        // HYBRID BRAIN INTEGRATION: C++ manages logic, JS handles LLM network calls
        std::string extraContext;
        
        // 1. Analyze Task
        const std::string promptLower = lowerAscii(currentRequest.prompt);
        const bool isRetry = currentRequest.contextPayload.contains("apply_patch_retry") || promptLower.find("previous apply_patch attempt") != std::string::npos;
        const bool isArchitecture = promptLower.find("architecture") != std::string::npos || promptLower.find("refactor") != std::string::npos;
        const nlohmann::ordered_json payload =
            currentRequest.contextPayload.is_object()
                ? currentRequest.contextPayload
                : nlohmann::ordered_json::object();
        const std::string fileHint = actionSourceFileHint(action);
        const std::string workspaceHint =
            payloadStringValue(payload, "workspace_root");
        const std::string selectedText =
            payloadStringValue(payload, "selected_text");
        const std::string symbolHint =
            payloadStringValue(payload, "symbol");
        const std::vector<std::string> selectedFiles =
            payloadStringArrayValue(payload, "selected_files");
        const std::vector<std::string> fileTargets =
            payloadStringArrayValue(payload, "file_targets");
        
        std::cerr << "[ULTRA-CORE] ContextStrategy=" << (isRetry ? "RetryPatch" : (isArchitecture ? "Architecture" : "Standard")) << "\n";
        std::cerr << "[ULTRA-CORE] Received workspace="
                  << (workspaceHint.empty() ? stateManager_.projectRoot().generic_string()
                                            : workspaceHint)
                  << "\n";
        std::cerr << "[ULTRA-CORE] Received activeFile="
                  << (fileHint.empty() ? "<none>" : fileHint) << "\n";
        std::cerr << "[ULTRA-CORE] Received selectedFiles="
                  << selectedFiles.size() << "\n";
        std::cerr << "[ULTRA-CORE] Received selectedText bytes="
                  << selectedText.size() << "\n";
        
        ultra::cli::CommandOptions dummyOptions;
        dummyOptions.jsonOutput = true;
        std::size_t totalContextBytes = 0;
        const std::size_t maxContextBytes = 64000; // token budget safety
        const std::size_t maxCharsPerSource = 12000;

        const auto appendContextBlock = [&](const std::string& label,
                                            std::string content) {
          content = trimAscii(std::move(content));
          if (content.empty() || totalContextBytes >= maxContextBytes) {
            return false;
          }
          if (content.size() > maxCharsPerSource) {
            content.resize(maxCharsPerSource);
            content += "\n... [truncated]";
          }
          if (content.size() > maxContextBytes - totalContextBytes) {
            content.resize(maxContextBytes - totalContextBytes);
          }
          totalContextBytes += content.size();
          extraContext += "\n\n=== " + label + " ===\n" + content;
          return true;
        };

        const auto addCommandContext = [&](const std::string& cmd,
                                           const std::string& label) {
          if (totalContextBytes >= maxContextBytes) {
            return false;
          }
          std::cerr << "[ULTRA-CORE] Running ultra " << cmd << "\n";
          std::string out;
          const int exitCode = ultra::adapters::runCommand(
              stateManager_.projectRoot(),
              "ultra " + cmd,
              dummyOptions,
              &out);
          if (exitCode == 0 && appendContextBlock(label, out)) {
            std::cerr << "[ULTRA-CORE] " << cmd << " returned "
                      << out.size() << " bytes\n";
            return true;
          }
          std::cerr << "[ULTRA-CORE] " << cmd << " failed rc=" << exitCode
                    << " bytes=" << out.size() << "\n";
          return false;
        };

        std::vector<std::string> candidateFiles;
        const auto addCandidateFile = [&](const std::string& value) {
          const std::string normalized = normalizePathToken(trimAscii(value));
          if (!normalized.empty()) {
            candidateFiles.push_back(normalized);
          }
        };
        addCandidateFile(fileHint);
        for (const std::string& file : selectedFiles) {
          addCandidateFile(file);
        }
        for (const std::string& file : fileTargets) {
          addCandidateFile(file);
        }
        if (payload.contains("inline_files") && payload.at("inline_files").is_array()) {
          for (const auto& item : payload.at("inline_files")) {
            if (item.is_object() && item.contains("path") && item.at("path").is_string()) {
              addCandidateFile(item.at("path").get<std::string>());
            }
          }
        }
        sortAndDedupe(candidateFiles);

        const auto readWorkspaceFile = [&](const std::string& candidate) {
          if (candidate.empty()) {
            return std::string{};
          }
          std::filesystem::path raw(candidate);
          std::filesystem::path absolute =
              raw.is_absolute() ? raw.lexically_normal()
                                : (stateManager_.projectRoot() / raw).lexically_normal();
          const std::filesystem::path relative =
              absolute.lexically_relative(stateManager_.projectRoot());
          const std::string relativeText = relative.generic_string();
          if (relative.empty() ||
              (relativeText.size() >= 2U && relativeText[0] == '.' &&
               relativeText[1] == '.')) {
            return std::string{};
          }
          return slurpTextFile(absolute);
        };

        const auto addDirectFileContext = [&](const std::string& candidate,
                                              const std::string& labelPrefix) {
          const std::string content = readWorkspaceFile(candidate);
          if (content.empty()) {
            std::cerr << "[ULTRA-CORE] direct file read failed: " << candidate
                      << "\n";
            return false;
          }
          if (appendContextBlock(labelPrefix + ": " + candidate, content)) {
            std::cerr << "[ULTRA-CORE] direct file read returned "
                      << content.size() << " bytes for " << candidate << "\n";
            return true;
          }
          return false;
        };

        const auto addInlineFileContext = [&](const std::string& candidate) {
          if (!payload.contains("inline_files") || !payload.at("inline_files").is_array()) {
            return false;
          }
          for (const auto& item : payload.at("inline_files")) {
            if (!item.is_object() || !item.contains("path") ||
                !item.at("path").is_string() || !item.contains("content") ||
                !item.at("content").is_string()) {
              continue;
            }
            const std::string inlinePath =
                normalizePathToken(item.at("path").get<std::string>());
            if (inlinePath != candidate) {
              continue;
            }
            const std::string content = item.at("content").get<std::string>();
            if (appendContextBlock("Inline File Snapshot: " + inlinePath, content)) {
              std::cerr << "[ULTRA-CORE] inline file snapshot returned "
                        << content.size() << " bytes for " << inlinePath << "\n";
              return true;
            }
          }
          return false;
        };

        if (!isRetry) {
           std::cerr << "[ULTRA-CORE] Running ultra scan (Refresh index)\n";
           std::string dummyOut;
           ultra::adapters::runCommand(stateManager_.projectRoot(), "ultra scan", dummyOptions, &dummyOut);
        }

        if (isArchitecture) {
           addCommandContext("graph", "Project Architecture Graph");
           if (!action.target.empty() && action.target != "workspace_root") {
               addCommandContext("context --query " + quoteForShell(action.target), "Semantic Query: " + action.target);
           }
        }

        if (isRetry) {
           if (!candidateFiles.empty()) {
               addCommandContext("context-diff " + quoteForShell(candidateFiles.front()), "Recent Changes in " + candidateFiles.front());
           }
           
           // Heuristic to extract undefined symbols from compiler errors
           std::string undefinedSymbol;
           auto symPos = currentRequest.prompt.find("undefined reference to");
           if (symPos != std::string::npos) {
               auto startQuote = currentRequest.prompt.find('`', symPos);
               auto endQuote = currentRequest.prompt.find('\'', startQuote != std::string::npos ? startQuote + 1 : symPos);
               if (startQuote != std::string::npos && endQuote != std::string::npos && endQuote > startQuote) {
                   undefinedSymbol = currentRequest.prompt.substr(startQuote + 1, endQuote - startQuote - 1);
               }
           } else if ((symPos = currentRequest.prompt.find("was not declared in this scope")) != std::string::npos) {
                auto startQuote = currentRequest.prompt.rfind('\'', symPos);
                if (startQuote != std::string::npos) {
                    auto realStart = currentRequest.prompt.rfind('\'', startQuote - 1);
                    if (realStart != std::string::npos) {
                        undefinedSymbol = currentRequest.prompt.substr(realStart + 1, startQuote - realStart - 1);
                    }
                }
           }
           
           if (!undefinedSymbol.empty()) {
               addCommandContext("ai_query " + quoteForShell(undefinedSymbol), "Symbol Query: " + undefinedSymbol);
            }
        }

        for (const std::string& candidate : candidateFiles) {
           std::cerr << "[ULTRA-CORE] Trying ai_source for " << candidate << "...\n";
           const bool aiSourceOk =
               addCommandContext("ai_source " + quoteForShell(candidate),
                                 "Source Code: " + candidate);
            if (!aiSourceOk) {
              std::cerr << "[ULTRA-CORE] ai_source failed: " << candidate << "\n";
              std::cerr << "[ULTRA-CORE] Falling back to direct file read\n";
              if (!addDirectFileContext(candidate, "Direct File Read")) {
                addInlineFileContext(candidate);
              }
            }
            if (!isRetry && !isArchitecture) {
              const std::string astTarget =
                  astContextTargetForPath(stateManager_.projectRoot(), candidate);
              addCommandContext("context --ast " + quoteForShell(astTarget),
                                "AST Summary: " + astTarget);
            }
            if (totalContextBytes >= maxContextBytes) {
              break;
            }
        }

        if (!selectedText.empty()) {
          appendContextBlock("Selected Text", selectedText);
        }
        if (!symbolHint.empty()) {
          addCommandContext("ai_query " + quoteForShell(symbolHint),
                            "Symbol Query: " + symbolHint);
        }
        if (totalContextBytes == 0) {
          const std::string semanticTarget = !candidateFiles.empty()
              ? candidateFiles.front()
              : (!symbolHint.empty() ? symbolHint : action.target);
          if (!semanticTarget.empty() && semanticTarget != "workspace_root") {
            addCommandContext("ai_context " + quoteForShell(semanticTarget),
                              "Semantic Slice: " + semanticTarget);
          } else {
            addCommandContext("context --ast .", "Workspace Semantic Slice");
          }
        }
        
        std::cerr << "[ULTRA-CORE] Final prompt chars=" << (currentRequest.prompt.size() + extraContext.size()) << "\n";
        std::cerr << "[ULTRA-CORE] Final context bytes=" << totalContextBytes << "\n";
        std::cout << nlohmann::ordered_json{{"type", "status"}, {"message", "ULTRA assembled " + std::to_string(totalContextBytes) + " bytes of native context."}}.dump() << std::endl;

        nlohmann::ordered_json messages = nlohmann::ordered_json::array();
        if (!currentRequest.systemPrompt.empty()) {
          messages.push_back({{"role", "system"}, {"content", currentRequest.systemPrompt}});
        }
        messages.push_back({{"role", "user"}, {"content", currentRequest.prompt + extraContext}});

        nlohmann::ordered_json llmReq = nlohmann::ordered_json::object();
        llmReq["type"] = "llm_request";
        llmReq["purpose"] = "patch_generation";
        llmReq["messages"] = std::move(messages);
        nlohmann::ordered_json meta = nlohmann::ordered_json::object();
        meta["mode"] = "patch";
        llmReq["metadata"] = std::move(meta);

        std::cerr << "[ULTRA-CORE] Need LLM for patch_generation\n";
        ai::model::ModelResponse response;
        std::string selectedProvider = "auto";
        std::vector<std::string> attemptedProviders;
        std::string providerEndpoint;

        bool hasJsBridge = false;
#ifdef _MSC_VER
        char* buf = nullptr;
        size_t sz = 0;
        if (_dupenv_s(&buf, &sz, "ULTRA_JS_BRIDGE") == 0 && buf != nullptr) {
            hasJsBridge = true;
            free(buf);
        }
#else
        hasJsBridge = (std::getenv("ULTRA_JS_BRIDGE") != nullptr);
#endif

        if (!hasJsBridge && modelOrchestrator_) {
            response = modelOrchestrator_->generate(currentRequest, routedContext);
            selectedProvider = action.modelProvider;
            if (const auto* orchestrator =
                    dynamic_cast<const ai::orchestration::MultiModelOrchestrator*>(
                        modelOrchestrator_.get());
                orchestrator != nullptr) {
              const ai::orchestration::OrchestrationDecision& decision =
                  orchestrator->lastDecision();
              if (!decision.selectedProvider.empty()) {
                selectedProvider = decision.selectedProvider;
              }
              attemptedProviders = decision.attemptedProviders;
            }
            providerEndpoint = providerEndpointForProject(stateManager_.projectRoot(), selectedProvider);
        } else {
            std::cout << llmReq.dump() << std::endl;

            std::string responseLine;
            if (!std::getline(std::cin, responseLine)) {
              response.ok = false;
              response.errorMessage = "Failed to read LLM response from JS bridge.";
            } else {
              try {
                nlohmann::ordered_json jsResp = nlohmann::ordered_json::parse(responseLine);
                if (jsResp.value("type", "") == "llm_error") {
                  response.ok = false;
                  response.errorMessage = jsResp.value("message", "Unknown LLM error from JS layer.");
                } else {
                  response.ok = true;
                  response.textOutput = jsResp.value("content", "");
                }
              } catch (...) {
                response.ok = false;
                response.errorMessage = "Failed to parse LLM response JSON from JS bridge.";
              }
            }
            selectedProvider = "js-bridge";
            attemptedProviders = {selectedProvider};
            providerEndpoint = "hybrid";
        }
        lastSelectedProvider_ = selectedProvider;
        lastProviderEndpoint_ = providerEndpoint;
        lastModelTextOutput_ = response.textOutput;
        result.text_output = lastModelTextOutput_;

        std::cerr << "[ULTRA-DEBUG] ModelGenerate response."
                  << " ok=" << (response.ok ? "true" : "false")
                  << " error=" << response.errorMessage
                  << " output_len=" << response.textOutput.size()
                  << "\n";
        if (!selectedProvider.empty()) {
          std::cerr << "[ULTRA-RUNTIME] provider_used=" << selectedProvider;
          if (!providerEndpoint.empty()) {
            std::cerr << " endpoint=" << providerEndpoint;
          }
          std::cerr << "\n";
        }

        result.payload = buildModelExecutionJson(routedContext,
                                                 action.modelProvider,
                                                 currentRequest, response);
        if (!selectedProvider.empty()) {
          result.payload["selected_provider"] = selectedProvider;
        }
        if (!attemptedProviders.empty()) {
          result.payload["attempted_providers"] = attemptedProviders;
        }
        if (!providerEndpoint.empty()) {
          result.payload["provider_endpoint"] = providerEndpoint;
        }
        if (!result.text_output.empty()) {
          result.payload["text_output"] = result.text_output;
        }
        result.normalizedPaths = collectNormalizedPaths(result.payload);
        result.risk = response.ok ? RiskLevel::Low : RiskLevel::Medium;
        result.ok = response.ok;
        if (!response.ok) {
          result.message = response.errorMessage.empty()
                               ? "Model generation failed."
                               : response.errorMessage;
          break;
        }

        const bool providerSuppliedToolCalls = !response.toolCalls.empty();
        std::vector<ai::model::ToolCall> toolCalls = response.toolCalls;
        if (toolCalls.empty()) {
          toolCalls = extractToolCallsFromText(response.textOutput);
          if (!toolCalls.empty()) {
            result.payload["tool_calls_inferred"] = true;
          }
        }
        if (toolCalls.empty()) {
          result.ok = false;
          result.risk = RiskLevel::High;
          result.message = "Model generation returned no executable tool calls.";
          break;
        }

        const bool retryableApplyPatchSequence =
            allToolCallsUseApplyPatch(toolCalls);
        nlohmann::ordered_json detectedToolCalls =
            nlohmann::ordered_json::array();
        for (const ai::model::ToolCall& toolCall : toolCalls) {
          detectedToolCalls.push_back(
              {{"tool", normalizeToolName(toolCall.name)},
               {"arguments", toolCall.arguments}});
        }
        result.payload["tool_call_detected"] = true;
        result.payload["tool_call_count"] = toolCalls.size();
        result.payload["tool_call_source"] =
            providerSuppliedToolCalls ? "provider" : "text";
        result.payload["tool_calls_detected"] = std::move(detectedToolCalls);
        if (!response.textOutput.empty()) {
          result.payload["model_text_output"] = response.textOutput;
        }
        result.payload.erase("text_output");
        result.text_output.clear();
        lastModelTextOutput_.clear();
        std::cerr << "[TOOL_CALL_DETECTED] count=" << toolCalls.size()
                  << " source="
                  << (providerSuppliedToolCalls ? "provider" : "text")
                  << " tools=" << summarizeToolCallNames(toolCalls) << "\n";

        nlohmann::ordered_json toolResults = nlohmann::ordered_json::array();
        bool toolFailure = false;
        bool retryRequested = false;
        std::string toolFailureMessage;
        for (std::size_t index = 0U; index < toolCalls.size(); ++index) {
          const ai::model::ToolCall& toolCall = toolCalls[index];
          Action toolAction;
          toolAction.type = ActionType::ToolExecution;
          toolAction.id = action.id + ":tool:" + std::to_string(index + 1U);
          toolAction.target = action.target;
          toolAction.branch = action.branch;
          toolAction.snapshotVersion = action.snapshotVersion;
          toolAction.toolName = normalizeToolName(toolCall.name);
          toolAction.toolArgs = orderedJsonToStringMap(toolCall.arguments);
          if (toolAction.toolName == "apply_patch" &&
              toolAction.toolArgs.find("project_path") ==
                  toolAction.toolArgs.end()) {
            toolAction.toolArgs["project_path"] =
                stateManager_.projectRoot().string();
          }
          if (toolAction.toolName.empty()) {
            toolFailure = true;
            toolFailureMessage =
                "Model generated a tool call without a tool name.";
            break;
          }

          Result childResult = executeActionLocked(toolAction, state);
          updateToolFailureHistory(toolAction, childResult);
          toolResults.push_back({
              {"args", toolAction.toolArgs},
              {"impacted_nodes", childResult.impactedNodes},
              {"message", childResult.message},
              {"normalized_paths", childResult.normalizedPaths},
              {"ok", childResult.ok},
              {"payload", childResult.payload},
              {"risk", toString(childResult.risk)},
              {"text_output", childResult.text_output},
              {"tool", toolAction.toolName},
          });
          result.impactedNodes.insert(result.impactedNodes.end(),
                                      childResult.impactedNodes.begin(),
                                      childResult.impactedNodes.end());
          result.normalizedPaths.insert(result.normalizedPaths.end(),
                                        childResult.normalizedPaths.begin(),
                                        childResult.normalizedPaths.end());
          result.risk = maxRisk(result.risk, childResult.risk);
          result.applied = result.applied || childResult.applied;
          if (childResult.payload.is_object() &&
              childResult.payload.contains("output_json") &&
              childResult.payload.at("output_json").is_object()) {
            result.payload["tool_execution"] =
                childResult.payload.at("output_json");
            if (!result.payload["tool_execution"].contains("tool")) {
              result.payload["tool_execution"]["tool"] = toolAction.toolName;
            }
          }
          if (childResult.payload.is_object() &&
              childResult.payload.contains("tool_router_executed") &&
              childResult.payload.at("tool_router_executed").is_boolean() &&
              childResult.payload.at("tool_router_executed").get<bool>()) {
            result.payload["tool_router_executed"] = true;
          }
          if (childResult.payload.is_object() &&
              childResult.payload.contains("tool_router_transport") &&
              childResult.payload.at("tool_router_transport").is_string()) {
            result.payload["tool_router_transport"] =
                childResult.payload.at("tool_router_transport")
                    .get<std::string>();
          }

          const bool applyPatchFailure =
              toolAction.toolName == "apply_patch" &&
              applyPatchFailureDetected(childResult);
          if (applyPatchFailure) {
            toolFailure = true;
            toolFailureMessage = describeApplyPatchFailure(childResult);
            if (retryableApplyPatchSequence &&
                applyPatchRetryCount < kApplyPatchMaxRetries) {
              ++applyPatchRetryCount;
              applyPatchRetryHistory.push_back(buildApplyPatchRetryRecord(
                  applyPatchRetryCount,
                  toolFailureMessage,
                  toolAction.toolArgs,
                  childResult));
              currentRequest = buildApplyPatchRetryRequest(currentRequest,
                                                          applyPatchRetryCount,
                                                          toolFailureMessage,
                                                          toolAction.toolArgs,
                                                          childResult);
              retryRequested = true;
            } else if (retryableApplyPatchSequence) {
              applyPatchRetryExhausted = true;
            }
            break;
          }

          if (!childResult.ok) {
            toolFailure = true;
            toolFailureMessage = childResult.message;
            break;
          }
        }

        result.payload["tool_results"] = std::move(toolResults);
        result.payload["tool_execution_summary"] =
            summarizeExecutedTools(toolCalls);
        sortAndDedupe(result.impactedNodes);
        result.normalizedPaths =
            normalizeAndSortPaths(std::move(result.normalizedPaths));
        if (retryRequested) {
          result.payload["retry_pending"] = true;
          result.payload["retry_reason"] = toolFailureMessage;
          continue;
        }
        if (toolFailure) {
          result.ok = false;
          result.message = toolFailureMessage.empty()
                               ? "Tool execution failed after model generation."
                               : toolFailureMessage;
          break;
        }

        result.ok = true;
        result.text_output.clear();
        result.payload.erase("text_output");
        lastModelTextOutput_.clear();
        result.message = applyPatchRetryCount == 0U
                             ? "Model generation completed and tool calls executed."
                             : "Model generation completed and tool calls executed "
                               "after apply_patch retry.";
        break;
      }

      result.payload["apply_patch_retry_count"] = applyPatchRetryCount;
      if (!applyPatchRetryHistory.empty()) {
        result.payload["apply_patch_retry_history"] = applyPatchRetryHistory;
      }
      if (applyPatchRetryExhausted) {
        result.payload["apply_patch_retry_exhausted"] = true;
      }
      result.normalizedPaths =
          normalizeAndSortPaths(collectNormalizedPaths(result.payload));
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
      bool toolRouterExecuted = false;
      std::string routerTransport = "unavailable";
      const std::string normalizedTool = normalizeToolName(action.toolName);
      if (toolExecutor_.registry().get_tool(normalizedTool) != nullptr) {
        routerTransport = "tool_executor";
        toolOutput = toolExecutor_.execute(normalizedTool, action.toolArgs);
        toolSucceeded = toolExecutor_.last_execution_succeeded();
        toolRouterExecuted = true;
      } else {
        toolOutput = "ERROR: tool '" + normalizedTool + "' is not registered.";
      }
      result.payload["tool_router_executed"] = toolRouterExecuted;
      result.payload["tool_router_transport"] = routerTransport;
      if (toolRouterExecuted) {
        std::cerr << "[TOOL_ROUTER_EXECUTED] tool=" << normalizedTool
                  << " transport=" << routerTransport
                  << " ok=" << (toolSucceeded ? "true" : "false") << "\n";
      }
      result.payload["output"] = toolOutput;
      if (std::optional<nlohmann::ordered_json> parsedOutput =
              parseToolOutputJson(toolOutput);
          parsedOutput.has_value()) {
        result.payload["output_json"] = *parsedOutput;
        if (parsedOutput->is_object()) {
          result.payload["tool_execution"] = *parsedOutput;
          result.payload["tool_execution"]["tool"] = normalizedTool;
          if (parsedOutput->contains("ok") && (*parsedOutput)["ok"].is_boolean()) {
            toolSucceeded = (*parsedOutput)["ok"].get<bool>();
          }
          if (parsedOutput->contains("applied") &&
              (*parsedOutput)["applied"].is_boolean()) {
            result.applied = (*parsedOutput)["applied"].get<bool>();
          }
          if (parsedOutput->contains("file_verified") &&
              (*parsedOutput)["file_verified"].is_boolean()) {
            result.payload["file_verified"] =
                (*parsedOutput)["file_verified"].get<bool>();
          }
          if (parsedOutput->contains("error") &&
              (*parsedOutput)["error"].is_string()) {
            result.payload["error"] = (*parsedOutput)["error"].get<std::string>();
            if (!toolSucceeded) {
              result.message = (*parsedOutput)["error"].get<std::string>();
            }
          }
        }
      }
      if (!result.applied && normalizedTool == "apply_patch") {
        result.applied = toolSucceeded;
      }
      result.payload["applied"] = result.applied;
      result.payload["ok"] = toolSucceeded;
      if (normalizedTool == "apply_patch" || result.applied ||
          (result.payload.contains("file_verified") &&
           result.payload.at("file_verified").is_boolean())) {
        std::cerr << "[EXECUTION_KERNEL_APPLIED] tool=" << normalizedTool
                  << " ok=" << (toolSucceeded ? "true" : "false")
                  << " applied=" << (result.applied ? "true" : "false");
        if (result.payload.contains("file_verified") &&
            result.payload.at("file_verified").is_boolean()) {
          std::cerr << " file_verified="
                    << (result.payload.at("file_verified").get<bool>()
                            ? "true"
                            : "false");
        }
        std::cerr << "\n";
      }
      result.normalizedPaths = collectNormalizedPaths(result.payload);
      result.ok = toolSucceeded;
      result.risk = result.ok ? decision.risk : maxRisk(decision.risk, RiskLevel::High);
      if (result.message.empty()) {
        result.message = result.ok ? "Tool execution completed deterministically."
                                   : toolOutput;
      }
      break;
    }

    case ActionType::IntentEvaluation: {
      intent::IntentEvaluator evaluator;
      const intent::IntentEvaluation evaluation =
          evaluator.evaluateIntent(*action.intentRequest, state);
      result.payload["intent"] = buildIntentJson(evaluation.normalizedIntent);
      result.payload["ordered_tasks"] = evaluation.orderedTasks;
      result.payload["child_results"] = nlohmann::ordered_json::array();

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

      result.payload["execution_deferred_to_task_graph"] = true;
      result.risk = toRiskLevel(evaluation.bestPlan.riskClassification);
      result.ok = true;
      result.message =
          "Intent plan evaluated deterministically; execution deferred to TaskGraph.";
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

  try {
    if (action.id.empty()) {
      throw ::ultra::runtime::contracts::ContractViolationException({
          ::ultra::runtime::contracts::LayerId::L15_EXECUTION_KERNEL,
          ::ultra::runtime::contracts::ViolationType::ExecutionBypass,
          "ExecutionKernel::execute",
          "execution action is missing a task-graph task id",
          ::ultra::runtime::contracts::ContractValidator::currentPhase(),
      });
    }
    ::ultra::runtime::contracts::ContractValidator::assertTaskGraphExecution(
        action.id, "ExecutionKernel::execute");
    validateAction(action, state);
    CognitiveRuntime runtime(stateManager_);
    const std::string normalizedTool = normalizeToolName(action.toolName);
    if (action.type == ActionType::ToolExecution &&
        runtime.toolRegistry().get_tool(normalizedTool) == nullptr) {
      throw std::runtime_error(
          "Tool execution requested an unregistered tool in Layer 16.");
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

  updateFailureIntelligenceForAction(action, result);
  sortOutputs(result);
  lastResult_ = result;
  hasLastResult_ = true;
  if (!result.text_output.empty()) {
    std::cerr << "[ULTRA-DEBUG] Execution result text_output bytes="
              << result.text_output.size() << "\n";
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

  const std::string toolName = normalizeToolName(action.toolName);
  if (result.ok) {
    toolFailureCounts_[toolName] = 0U;
    return;
  }

  ++toolFailureCounts_[toolName];
}

bool ExecutionKernel::hasToolCognitionLayer() const noexcept {
  static const char* const kRequiredTools[] = {
      "query_symbol",
      "read_source",
      "impact_analysis",
      "get_context",
      "get_status",
      "apply_patch",
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
