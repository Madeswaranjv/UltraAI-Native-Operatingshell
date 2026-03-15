#include "AiRuntimeManager.h"

#include "ultra/ipc/ultra_ipc_client.h"
#include "ultra/runtime/ultra_daemon.h"

#include "../ai/AgentExportBuilder.h"
#include "../ai/BinaryIndexReader.h"
#include "../ai/BinaryIndexWriter.h"
#include "../ai/DependencyTable.h"
#include "../ai/FileRegistry.h"
#include "../ai/Hashing.h"
#include "../ai/IntegrityManager.h"
#include "../ai/SemanticExtractor.h"
#include "../ai/SymbolTable.h"
#include "../authority/UltraAuthorityAPI.h"
#include "../core/state_manager.h"
#include "../engine/query/SymbolQueryEngine.h"
#include "../engine/scanner.h"
#include "../intelligence/BranchPersistence.h"
#include "../indexing/IndexingService.h"
#include "../memory/HotSlice.h"
#include "../memory/StateSnapshot.h"
#include "../metrics/PerformanceMetrics.h"
#include "../metacognition/MetaCognitiveOrchestrator.h"
#include "../runtime/ContextExtractor.h"
#include "../runtime/precision_invalidation.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace ultra::ai {

namespace {

// Returns the absolute path of the currently running executable.
// On Windows: uses GetModuleFileNameW.
// On Linux:   resolves /proc/self/exe.
// On macOS:   resolves /proc/self/exe is not available; falls back to argv[0]
//             style (not needed here — project targets Win/Linux only).
std::filesystem::path currentExecutablePath() {
#ifdef _WIN32
  wchar_t buffer[32768]{};
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer, 32768);
  if (length == 0 || length == 32768) {
    return {};
  }
  return std::filesystem::path(buffer);
#else
  // Linux — /proc/self/exe is a symlink to the real executable.
  char buffer[4096]{};
  const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1U);
  if (length <= 0) {
    return {};
  }
  buffer[length] = '\0';
  return std::filesystem::path(buffer);
#endif
}

bool printStatusSnapshot(const nlohmann::json& payload, const bool verbose) {
  const bool runtimeActive = payload.value("runtime_active", false);
  std::cout << "AI runtime: " << (runtimeActive ? "active" : "inactive") << '\n';
  std::cout << "Daemon PID: " << payload.value("daemon_pid", 0UL) << '\n';
  std::cout << "Files indexed: " << payload.value("files_indexed", 0U) << '\n';
  std::cout << "Symbols indexed: " << payload.value("symbols_indexed", 0U) << '\n';
  std::cout << "Dependencies indexed: " << payload.value("dependencies_indexed", 0U) << '\n';
  std::cout << "Pending changes: " << payload.value("pending_changes", 0U) << '\n';
  std::cout << "Graph nodes: " << payload.value("graph_nodes", 0U) << '\n';
  std::cout << "Graph edges: " << payload.value("graph_edges", 0U) << '\n';
  std::cout << "Memory usage (bytes): " << payload.value("memory_usage_bytes", 0U) << '\n';
  std::cout << "Schema version: " << payload.value("schema_version", 0U) << '\n';
  std::cout << "Index version: " << payload.value("index_version", 0U) << '\n';

  if (verbose) {
    const nlohmann::json health =
        payload.value("kernel_health", nlohmann::json::object());
    std::cout << "Kernel healthy: "
              << (health.value("healthy", false) ? "yes" : "no") << '\n';
  }

  return runtimeActive;
}

nlohmann::json buildMetaCognitivePayload(
    const ultra::metacognition::QueryMetrics& metrics) {
  nlohmann::json payload;
  payload["stability_score"] = metrics.stabilityScore;
  payload["drift_score"] = metrics.driftScore;
  payload["learning_velocity"] = metrics.learningVelocity;
  payload["predicted_next_command"] = metrics.predictedNextCommand;
  payload["query_token_budget"] = metrics.queryTokenBudget;
  payload["query_cache_capacity"] = metrics.queryCacheCapacity;
  payload["hot_slice_capacity"] = metrics.hotSliceCapacity;
  payload["branch_retention_hint"] = metrics.branchRetentionHint;

  const bool conservative = metrics.stabilityScore < 0.4;
  const bool exploratory = !conservative &&
                           metrics.learningVelocity < 0.2 &&
                           metrics.driftScore < 0.1;
  payload["conservative_mode"] = conservative;
  payload["exploratory_mode"] = exploratory;
  return payload;
}

std::string normalizePathString(const std::filesystem::path& path) {
  const auto u8 = path.generic_u8string();
  std::string out;
  out.reserve(u8.size());
  for (const auto ch : u8) {
    out.push_back(static_cast<char>(ch));
  }
  return out;
}

bool endsWith(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> dependencyCandidatesForReference(
    const std::string& reference,
    const std::string& currentFilePath) {
  static const std::vector<std::string> kCodeExtensions{
      "",            ".h",        ".hpp",      ".hh",   ".hxx",
      ".c",          ".cc",       ".cpp",      ".cxx",  ".js",
      ".jsx",        ".mjs",      ".cjs",      ".ts",   ".tsx",
      ".py",         "/index.js", "/index.ts", "/index.tsx",
      "/__init__.py"};

  std::vector<std::string> out;
  out.reserve(kCodeExtensions.size() * 2U);

  const std::filesystem::path currentParent =
      std::filesystem::path(currentFilePath).parent_path();

  const auto addVariants = [&](const std::filesystem::path& basePath) {
    for (const std::string& ext : kCodeExtensions) {
      if (ext.empty()) {
        out.push_back(normalizePathString(basePath.lexically_normal()));
      } else if (ext[0] == '/') {
        out.push_back(
            normalizePathString((basePath / ext.substr(1)).lexically_normal()));
      } else {
        std::filesystem::path variant = basePath;
        variant += ext;
        out.push_back(normalizePathString(variant.lexically_normal()));
      }
    }
  };

  if (!reference.empty() && reference[0] == '.') {
    addVariants((currentParent / reference).lexically_normal());
  } else {
    addVariants(std::filesystem::path(reference));
    addVariants((currentParent / reference).lexically_normal());
    if (reference.find('.') != std::string::npos &&
        reference.find('/') == std::string::npos) {
      std::string pythonModule = reference;
      std::replace(pythonModule.begin(), pythonModule.end(), '.', '/');
      addVariants(std::filesystem::path(pythonModule));
    }
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool resolveDependencyReference(
    const std::string& currentFilePath,
    const std::string& reference,
    const std::map<std::string, ultra::ai::FileRecord>& currentFilesByPath,
    std::string& resolvedPath) {
  if (reference.empty()) {
    return false;
  }

  if (currentFilesByPath.find(reference) != currentFilesByPath.end()) {
    resolvedPath = reference;
    return true;
  }

  const std::vector<std::string> candidates =
      dependencyCandidatesForReference(reference, currentFilePath);
  for (const std::string& candidate : candidates) {
    const auto exactIt = currentFilesByPath.find(candidate);
    if (exactIt != currentFilesByPath.end()) {
      resolvedPath = exactIt->first;
      return true;
    }
  }

  std::vector<std::string> suffixMatches;
  suffixMatches.reserve(currentFilesByPath.size());
  for (const auto& [path, file] : currentFilesByPath) {
    (void)file;
    if (path == reference || endsWith(path, "/" + reference) ||
        endsWith(path, reference)) {
      suffixMatches.push_back(path);
    }
  }
  if (suffixMatches.empty()) {
    return false;
  }

  std::sort(suffixMatches.begin(), suffixMatches.end());
  resolvedPath = suffixMatches.front();
  return true;
}

bool isDefinitionSymbol(const ultra::ai::SymbolRecord& symbol) {
  switch (symbol.symbolType) {
    case ultra::ai::SymbolType::Class:
    case ultra::ai::SymbolType::Function:
    case ultra::ai::SymbolType::EntryPoint:
    case ultra::ai::SymbolType::ReactComponent:
      return true;
    case ultra::ai::SymbolType::Unknown:
    case ultra::ai::SymbolType::Import:
    case ultra::ai::SymbolType::Export:
    default:
      return false;
  }
}

std::unordered_map<std::string, ultra::ai::SymbolNode> buildSymbolIndex(
    const std::vector<ultra::ai::FileRecord>& files,
    const std::vector<ultra::ai::SymbolRecord>& symbols,
    const ultra::ai::DependencyTableData& deps) {
  std::unordered_map<std::string, ultra::ai::SymbolNode> index;
  index.reserve(symbols.size());
  if (symbols.empty()) {
    return index;
  }

  std::unordered_map<std::uint32_t, std::string> pathByFileId;
  pathByFileId.reserve(files.size());
  for (const ultra::ai::FileRecord& file : files) {
    pathByFileId[file.fileId] = file.path;
  }

  std::unordered_map<std::uint64_t, const ultra::ai::SymbolRecord*> symbolById;
  symbolById.reserve(symbols.size());
  for (const ultra::ai::SymbolRecord& symbol : symbols) {
    symbolById[symbol.symbolId] = &symbol;
  }

  std::unordered_map<std::string, std::vector<const ultra::ai::SymbolRecord*>>
      definitionsByName;
  definitionsByName.reserve(symbols.size());

  std::unordered_map<std::string, std::set<std::string>> usageFilesByName;
  usageFilesByName.reserve(symbols.size());

  for (const ultra::ai::SymbolRecord& symbol : symbols) {
    if (symbol.name.empty()) {
      continue;
    }

    const auto pathIt = pathByFileId.find(symbol.fileId);
    if (pathIt == pathByFileId.end()) {
      continue;
    }

    if (isDefinitionSymbol(symbol)) {
      definitionsByName[symbol.name].push_back(&symbol);
    } else {
      usageFilesByName[symbol.name].insert(pathIt->second);
    }
  }

  for (const ultra::ai::SymbolDependencyEdge& edge : deps.symbolEdges) {
    const auto fromIt = symbolById.find(edge.fromSymbolId);
    const auto toIt = symbolById.find(edge.toSymbolId);
    if (fromIt == symbolById.end() || toIt == symbolById.end()) {
      continue;
    }
    if (toIt->second == nullptr || toIt->second->name.empty()) {
      continue;
    }

    const auto userFileIt = pathByFileId.find(fromIt->second->fileId);
    if (userFileIt == pathByFileId.end()) {
      continue;
    }

    usageFilesByName[toIt->second->name].insert(userFileIt->second);
  }

  for (const auto& [name, definitions] : definitionsByName) {
    if (name.empty() || definitions.empty()) {
      continue;
    }

    std::string definedIn;
    for (const ultra::ai::SymbolRecord* definition : definitions) {
      if (definition == nullptr) {
        continue;
      }
      const auto pathIt = pathByFileId.find(definition->fileId);
      if (pathIt == pathByFileId.end()) {
        continue;
      }
      if (definedIn.empty() || pathIt->second < definedIn) {
        definedIn = pathIt->second;
      }
    }
    if (definedIn.empty()) {
      continue;
    }

    ultra::ai::SymbolNode node;
    node.name = name;
    node.definedIn = std::move(definedIn);
    const auto usageIt = usageFilesByName.find(name);
    if (usageIt != usageFilesByName.end()) {
      const auto& usage = usageIt->second;
      node.usedInFiles.insert(usage.begin(), usage.end());
    }
    index.emplace(name, std::move(node));
  }

  const double centralityDenom =
      files.size() > 1U ? static_cast<double>(files.size() - 1U) : 1.0;
  for (auto& [name, node] : index) {
    (void)name;
    node.centrality = files.size() > 1U
                          ? static_cast<double>(node.usedInFiles.size()) /
                                centralityDenom
                          : 0.0;
    node.weight = 1.0 + (0.25 * node.centrality);
  }

  return index;
}

void rebuildSymbolEdges(ultra::ai::RuntimeState& state) {
  ultra::ai::SymbolTable::sortDeterministic(state.symbols);
  ultra::ai::DependencyTable::sortAndDedupe(state.deps);
  const std::map<std::uint32_t, std::vector<ultra::ai::SymbolRecord>> symbolsByFileId =
      ultra::ai::SymbolTable::groupByFileId(state.symbols);
  state.deps.symbolEdges.clear();
  const std::vector<ultra::ai::SymbolDependencyEdge> fromFileEdges =
      ultra::ai::DependencyTable::buildSymbolEdgesFromFileEdges(
          state.deps.fileEdges, symbolsByFileId);
  const std::vector<ultra::ai::SymbolDependencyEdge> fromSemanticEdges =
      ultra::ai::DependencyTable::buildSymbolEdgesFromSemanticDependencies(
          state.semanticSymbolDepsByFileId, symbolsByFileId);
  state.deps.symbolEdges.insert(state.deps.symbolEdges.end(),
                                fromFileEdges.begin(), fromFileEdges.end());
  state.deps.symbolEdges.insert(state.deps.symbolEdges.end(),
                                fromSemanticEdges.begin(),
                                fromSemanticEdges.end());
  ultra::ai::DependencyTable::sortAndDedupe(state.deps);
  state.symbolIndex = buildSymbolIndex(state.files, state.symbols, state.deps);
}

std::vector<std::string> toSortedVector(
    const std::unordered_set<std::string>& values) {
  std::vector<std::string> out(values.begin(), values.end());
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::size_t estimateRuntimeBytes(const ultra::ai::RuntimeState& runtime) {
  std::size_t total = sizeof(runtime);
  for (const auto& file : runtime.files) {
    total += sizeof(file) + file.path.size();
  }
  for (const auto& symbol : runtime.symbols) {
    total += sizeof(symbol) + symbol.name.size() + symbol.signature.size();
  }
  total += runtime.deps.fileEdges.size() * sizeof(ultra::ai::FileDependencyEdge);
  total += runtime.deps.symbolEdges.size() *
           sizeof(ultra::ai::SymbolDependencyEdge);
  for (const auto& [fileId, deps] : runtime.semanticSymbolDepsByFileId) {
    total += sizeof(fileId);
    for (const auto& dep : deps) {
      total += sizeof(dep) + dep.fromSymbol.size() + dep.toSymbol.size();
    }
  }
  for (const auto& [name, node] : runtime.symbolIndex) {
    total += name.size() + node.definedIn.size();
    for (const auto& usedIn : node.usedInFiles) {
      total += usedIn.size();
    }
  }
  return total;
}

nlohmann::json buildBranchPayload(const ultra::intelligence::Branch& branch) {
  nlohmann::json payload;
  payload["branchId"] = branch.branchId;
  payload["parentId"] = branch.parentId;
  payload["parentBranchId"] = branch.parentBranchId;
  payload["goal"] = branch.goal;
  payload["currentExecutionNodeId"] = branch.currentExecutionNodeId;
  payload["subBranches"] = branch.subBranches;
  payload["memorySnapshotId"] = branch.memorySnapshotId;
  payload["dependencyReferences"] = branch.dependencyReferences;
  payload["confidence"] = {
      {"stabilityScore", branch.confidence.stabilityScore},
      {"riskAdjustedConfidence", branch.confidence.riskAdjustedConfidence},
      {"decisionReliabilityIndex", branch.confidence.decisionReliabilityIndex},
  };
  payload["status"] = ultra::intelligence::toString(branch.status);
  payload["isOverlayResident"] = branch.isOverlayResident;
  payload["creationSequence"] = branch.creationSequence;
  payload["lastMutationSequence"] = branch.lastMutationSequence;
  return payload;
}

nlohmann::json buildBranchListPayload(
    const std::filesystem::path& projectRoot) {
  nlohmann::json payload = nlohmann::json::object();
  nlohmann::json branches = nlohmann::json::array();

  ultra::intelligence::BranchStore store;
  ultra::intelligence::BranchPersistence persistence(projectRoot / ".ultra");
  const bool loaded = persistence.load(store);
  if (loaded) {
    std::vector<ultra::intelligence::Branch> items = store.getAll();
    std::sort(items.begin(), items.end(),
              [](const auto& left, const auto& right) {
                return left.branchId < right.branchId;
              });
    for (const auto& branch : items) {
      branches.push_back(buildBranchPayload(branch));
    }
  }

  payload["branches"] = std::move(branches);
  payload["loaded"] = loaded;
  return payload;
}

nlohmann::json buildBranchDiffPayload(
    const ultra::diff::semantic::BranchDiffReport& report) {
  nlohmann::json payload;
  payload["symbols"] = nlohmann::json::array();
  for (const auto& symbol : report.symbols) {
    payload["symbols"].push_back(
        {{"id", symbol.id},
         {"type", ultra::diff::semantic::toString(symbol.type)}});
  }
  payload["signatures"] = nlohmann::json::array();
  for (const auto& signature : report.signatures) {
    payload["signatures"].push_back(
        {{"id", signature.id},
         {"change", ultra::diff::semantic::toString(signature.change)}});
  }
  payload["dependencies"] = nlohmann::json::array();
  for (const auto& dependency : report.dependencies) {
    payload["dependencies"].push_back(
        {{"from", dependency.from},
         {"to", dependency.to},
         {"type", ultra::diff::semantic::toString(dependency.type)}});
  }
  payload["risk"] = ultra::diff::semantic::toString(report.overallRisk);
  payload["impactScore"] = report.impactScore;
  return payload;
}

nlohmann::json buildRiskReportPayload(
    const ultra::authority::AuthorityRiskReport& report) {
  nlohmann::json payload;
  payload["score"] = report.score;
  payload["removed_symbols"] = report.removedSymbols;
  payload["signature_changes"] = report.signatureChanges;
  payload["dependency_breaks"] = report.dependencyBreaks;
  payload["public_api_changes"] = report.publicApiChanges;
  payload["impact_depth"] = report.impactDepth;
  payload["within_threshold"] = report.withinThreshold;
  payload["diff_report"] = buildBranchDiffPayload(report.diffReport);
  return payload;
}

}  // namespace

struct RuntimeDispatcher {
  std::filesystem::path projectRoot;
  std::filesystem::path aiDirectory;
  std::filesystem::path agentContextPath;
  std::filesystem::path contextDiffPath;
  ultra::core::StateManager stateManager;
  ultra::indexing::IndexingService indexingService;
  std::optional<ultra::ai::RuntimeState> previousState;

  explicit RuntimeDispatcher(std::filesystem::path root)
      : projectRoot(std::filesystem::absolute(std::move(root)).lexically_normal()),
        aiDirectory(projectRoot / ".ultra" / "ai"),
        agentContextPath(aiDirectory / "agent_context.json"),
        contextDiffPath(projectRoot / ".ultra.context-diff.json"),
        stateManager(projectRoot),
        indexingService(projectRoot, stateManager) {}

  nlohmann::json handle(const std::string& type, const nlohmann::json& payload) {
    if (type == "ai_status") {
      const bool verbose = payload.value("verbose", false);
      return makeOk(buildStatusPayload(verbose), 0);
    }

    if (type == "rebuild_ai") {
      nlohmann::json payloadOut;
      std::string error;
      if (!rebuildIndex(payloadOut, error)) {
        return makeError(error);
      }
      return makeOk(payloadOut, 0, "rebuild_complete");
    }

    if (type == "ai_query") {
      const std::string target = payload.value("target", std::string{});
      if (target.empty()) {
        return makeError("missing_query_target");
      }
      std::string error;
      if (!ensureIndex(error)) {
        return makeError(error);
      }
      nlohmann::json result;
      if (!indexingService.queryTarget(stateManager.getSnapshot(), target, result, error)) {
        return makeError(error);
      }
      const int exitCode = result.value("kind", "") == "not_found" ? 1 : 0;
      return makeOk(result, exitCode);
    }

    if (type == "ai_source") {
      const std::string target = payload.value("file", std::string{});
      if (target.empty()) {
        return makeError("missing_source_target");
      }
      std::string error;
      if (!ensureIndex(error)) {
        return makeError(error);
      }
      nlohmann::json result;
      if (!indexingService.readSource(target, result, error)) {
        return makeError(error);
      }
      return makeOk(result, 0);
    }

    if (type == "ai_impact") {
      const std::string target = payload.value("target", std::string{});
      if (target.empty()) {
        return makeError("missing_impact_target");
      }
      std::string error;
      if (!ensureIndex(error)) {
        return makeError(error);
      }
      const std::size_t depth = std::max<std::size_t>(
          1U, payload.value("depth", payload.value("max_depth", 2U)));
      ultra::indexing::ImpactReport report;
      if (!indexingService.analyzeImpact(target, depth, report, error)) {
        return makeError(error);
      }
      const int exitCode = report.found ? 0 : 1;
      return makeOk(report.payload, exitCode);
    }

    if (type == "ai_context") {
      std::string error;
      if (!ensureIndex(error)) {
        return makeError(error);
      }
      nlohmann::json payloadOut;
      if (!writeAgentContext(payloadOut, error)) {
        return makeError(error);
      }
      if (payload.contains("query")) {
        payloadOut["query"] = payload.value("query", std::string{});
      }
      return makeOk(payloadOut, 0);
    }

    if (type == "context_diff") {
      nlohmann::json payloadOut;
      std::string error;
      if (!computeContextDiff(payloadOut, error)) {
        return makeError(error);
      }
      return makeOk(payloadOut, 0);
    }

    if (type == "ai_metrics" || type == "metrics") {
      const std::string action = payload.value("action", std::string{});
      if (action == "enable") {
        ultra::metrics::PerformanceMetrics::setEnabled(true);
      } else if (action == "disable") {
        ultra::metrics::PerformanceMetrics::setEnabled(false);
      } else if (action == "reset") {
        ultra::metrics::PerformanceMetrics::reset();
      }
      nlohmann::json payloadOut;
      payloadOut["report"] = ultra::metrics::PerformanceMetrics::report();
      payloadOut["meta_cognitive"] = nlohmann::json::object();
      return makeOk(payloadOut, 0);
    }

    if (type == "ai_verify") {
      nlohmann::json payloadOut;
      std::string error;
      if (!verifyIntegrity(payloadOut, error)) {
        return makeError(error);
      }
      return makeOk(payloadOut, 0);
    }

    if (type == "authority_branch_list") {
      return makeOk(buildBranchListPayload(projectRoot), 0);
    }

    if (type == "authority_branch_create") {
      ultra::authority::AuthorityBranchRequest request;
      request.reason = payload.value("reason", std::string{});
      request.parentBranchId = payload.value("parent_branch_id", std::string{});
      if (request.reason.empty()) {
        return makeError("missing_branch_reason");
      }
      try {
        ultra::authority::UltraAuthorityAPI api(projectRoot);
        const std::string branchId = api.createBranch(request);
        nlohmann::json payloadOut = nlohmann::json::object();
        payloadOut["branch_id"] = branchId;
        payloadOut["parent_branch_id"] = request.parentBranchId;
        payloadOut["reason"] = request.reason;
        return makeOk(payloadOut, 0);
      } catch (const std::exception& ex) {
        return makeError(ex.what());
      } catch (...) {
        return makeError("branch_create_failed");
      }
    }

    if (type == "authority_intent_simulate") {
      ultra::authority::AuthorityIntentRequest request;
      request.goal = payload.value("goal", std::string{});
      request.target = payload.value("target", std::string{});
      request.branchId = payload.value("branch_id", std::string{});
      request.tokenBudget = payload.value("token_budget", request.tokenBudget);
      request.impactDepth = payload.value("impact_depth", request.impactDepth);
      request.maxFilesChanged =
          payload.value("max_files_changed", request.maxFilesChanged);
      request.allowPublicApiChange =
          payload.value("allow_public_api_change", request.allowPublicApiChange);
      request.threshold = payload.value("threshold", request.threshold);
      if (request.goal.empty() && request.target.empty()) {
        return makeError("missing_intent_goal");
      }
      if (request.target.empty()) {
        request.target = request.goal;
      }
      try {
        ultra::authority::UltraAuthorityAPI api(projectRoot);
        const ultra::authority::AuthorityRiskReport report =
            api.evaluateRisk(request);
        return makeOk(buildRiskReportPayload(report), 0);
      } catch (const std::exception& ex) {
        return makeError(ex.what());
      } catch (...) {
        return makeError("intent_simulation_failed");
      }
    }

    if (type == "authority_context_query") {
      ultra::authority::AuthorityContextRequest request;
      request.query = payload.value("query", std::string{});
      request.branchId = payload.value("branch_id", std::string{});
      request.tokenBudget = payload.value("token_budget", request.tokenBudget);
      request.impactDepth = payload.value("impact_depth", request.impactDepth);
      if (request.query.empty()) {
        return makeError("missing_context_query");
      }
      ultra::authority::UltraAuthorityAPI api(projectRoot);
      const ultra::authority::AuthorityContextResult result =
          api.getContextSlice(request);
      if (!result.success) {
        return makeError(result.message.empty() ? "context_query_failed"
                                                : result.message);
      }
      nlohmann::json payloadOut = nlohmann::json::object();
      payloadOut["context_json"] = result.contextJson;
      payloadOut["estimated_tokens"] = result.estimatedTokens;
      payloadOut["snapshot_version"] = result.snapshotVersion;
      payloadOut["snapshot_hash"] = result.snapshotHash;
      payloadOut["message"] = result.message;
      return makeOk(payloadOut, 0);
    }

    if (type == "authority_commit" || type == "authority_branch_commit") {
      ultra::authority::AuthorityCommitRequest request;
      request.sourceBranchId = payload.value("source_branch_id", std::string{});
      request.targetBranchId =
          payload.value("target_branch_id", std::string{"main"});
      request.maxAllowedRisk =
          payload.value("max_allowed_risk", request.maxAllowedRisk);
      const nlohmann::json policyPayload =
          payload.value("policy", nlohmann::json::object());
      if (policyPayload.is_object()) {
        request.policy.maxImpactDepth =
            policyPayload.value("max_impact_depth", request.policy.maxImpactDepth);
        request.policy.maxFilesChanged =
            policyPayload.value("max_files_changed", request.policy.maxFilesChanged);
        request.policy.maxTokenBudget =
            policyPayload.value("max_token_budget", request.policy.maxTokenBudget);
        request.policy.allowPublicAPIChange =
            policyPayload.value("allow_public_api_change",
                                request.policy.allowPublicAPIChange);
        request.policy.allowCrossModuleMove =
            policyPayload.value("allow_cross_module_move",
                                request.policy.allowCrossModuleMove);
        request.policy.requireDeterminism =
            policyPayload.value("require_determinism",
                                request.policy.requireDeterminism);
      }
      if (request.sourceBranchId.empty()) {
        return makeError("missing_source_branch");
      }
      std::string error;
      ultra::authority::UltraAuthorityAPI api(projectRoot);
      if (!api.commitWithPolicy(request, error)) {
        return makeError(error.empty() ? "commit_failed" : error);
      }
      nlohmann::json payloadOut = nlohmann::json::object();
      payloadOut["committed"] = true;
      payloadOut["source_branch_id"] = request.sourceBranchId;
      payloadOut["target_branch_id"] = request.targetBranchId;
      return makeOk(payloadOut, 0);
    }

    if (type == "authority_savings") {
      ultra::authority::UltraAuthorityAPI api(projectRoot);
      nlohmann::json payloadOut = api.getSavingsReport();
      return makeOk(payloadOut, 0);
    }

    return makeError("unsupported_request_type");
  }

 private:
  static nlohmann::json makeOk(nlohmann::json payload,
                               const int exitCode,
                               const std::string& message = {}) {
    nlohmann::json response;
    response["status"] = "ok";
    response["payload"] = std::move(payload);
    response["exit_code"] = exitCode;
    response["ok"] = exitCode == 0;
    if (!message.empty()) {
      response["message"] = message;
    }
    return response;
  }

  static nlohmann::json makeError(const std::string& error) {
    return nlohmann::json{{"status", "error"}, {"error", error}};
  }

  bool ensureIndex(std::string& error) {
    if (indexingService.hasSemanticIndex()) {
      return true;
    }
    // Lazy first-use build: startup rebuild was deferred so the IPC server
    // could start and answer the parent's "wake" ping. Build now on first
    // query that actually needs the index.
    nlohmann::json payloadOut;
    return rebuildIndex(payloadOut, error);
  }

  std::uint64_t daemonPid() const {
    const std::filesystem::path pidPath =
        ultra::runtime::UltraDaemon::daemonPidFile(projectRoot);
    std::ifstream stream(pidPath);
    long long pid = 0;
    if (!(stream >> pid) || pid <= 0) {
      return 0U;
    }
    return static_cast<std::uint64_t>(pid);
  }

  static std::size_t estimateSnapshotBytes(
      const ultra::memory::StateSnapshot& snapshot) {
    std::size_t total =
        sizeof(snapshot.id) + sizeof(snapshot.nodeCount) +
        sizeof(snapshot.edgeCount) + snapshot.snapshotId.size() +
        snapshot.graphHash.size();

    for (const ultra::memory::StateNode& node : snapshot.nodes) {
      total += sizeof(node.nodeType) + sizeof(node.version) + node.nodeId.size() +
               node.data.dump().size();
    }
    for (const ultra::memory::StateEdge& edge : snapshot.edges) {
      total += sizeof(edge.edgeType) + sizeof(edge.weight) + edge.edgeId.size() +
               edge.sourceId.size() + edge.targetId.size();
    }

    return total;
  }

  nlohmann::json buildStatusPayload(const bool verbose) {
    const std::size_t pendingChanges = 0U;
    const ultra::core::RuntimeStatusSnapshot status =
        stateManager.snapshotStatus(pendingChanges);

    nlohmann::json payload = nlohmann::json::object();
    payload["runtime_active"] = status.runtimeActive;
    payload["daemon_pid"] = daemonPid();
    payload["files_indexed"] = status.filesIndexed;
    payload["symbols_indexed"] = status.symbolsIndexed;
    payload["dependencies_indexed"] = status.dependenciesIndexed;
    payload["pending_changes"] = status.pendingChanges;
    payload["schema_version"] = status.schemaVersion;
    payload["index_version"] = status.indexVersion;

    const ultra::runtime::GraphSnapshot snapshot = stateManager.getSnapshot();
    const std::size_t nodeCount = snapshot.graph ? snapshot.graph->nodeCount() : 0U;
    const std::size_t edgeCount = snapshot.graph ? snapshot.graph->edgeCount() : 0U;
    payload["graph_nodes"] = nodeCount;
    payload["graph_edges"] = edgeCount;

    std::size_t memoryBytes = 0U;
    if (snapshot.graph) {
      const ultra::memory::StateSnapshot graphSnapshot =
          snapshot.graph->snapshot(snapshot.version);
      memoryBytes = estimateSnapshotBytes(graphSnapshot);
    }
    payload["memory_usage_bytes"] = memoryBytes;

    if (verbose) {
      const ultra::core::KernelHealthSnapshot health =
          stateManager.verifyKernelHealth();
      nlohmann::json healthJson = nlohmann::json::object();
      healthJson["branch_count"] = health.branchCount;
      healthJson["snapshot_count"] = health.snapshotCount;
      healthJson["governance_active"] = health.governanceActive;
      healthJson["determinism_guards_active"] = health.determinismGuardsActive;
      healthJson["memory_caps_respected"] = health.memoryCapsRespected;
      healthJson["healthy"] = health.healthy;
      healthJson["violations"] = health.violations;
      payload["kernel_health"] = std::move(healthJson);
    }

    return payload;
  }

 public:
  bool rebuildIndex(nlohmann::json& payloadOut, std::string& error) {
    payloadOut = nlohmann::json::object();
    error.clear();

    std::optional<ultra::ai::RuntimeState> priorState;
    if (indexingService.hasSemanticIndex()) {
      priorState = stateManager.snapshotState();
    }

    ultra::engine::Scanner scanner(projectRoot);
    ultra::engine::ScanOutput output;
    if (!scanner.fullScanParallel(output, error)) {
      return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(aiDirectory, ec);
    if (ec) {
      error = "Failed to create AI runtime directory: " + aiDirectory.string();
      return false;
    }

    const std::filesystem::path filesTablePath = aiDirectory / "files.tbl";
    const std::filesystem::path symbolsTablePath = aiDirectory / "symbols.tbl";
    const std::filesystem::path depsTablePath = aiDirectory / "deps.tbl";
    const std::filesystem::path corePath = aiDirectory / "core.idx";

    if (!ultra::ai::BinaryIndexWriter::writeFilesTable(
            filesTablePath, ultra::ai::IntegrityManager::kSchemaVersion,
            output.files, error)) {
      return false;
    }
    if (!ultra::ai::BinaryIndexWriter::writeSymbolsTable(
            symbolsTablePath, ultra::ai::IntegrityManager::kSchemaVersion,
            output.symbols, error)) {
      return false;
    }
    if (!ultra::ai::BinaryIndexWriter::writeDependenciesTable(
            depsTablePath, ultra::ai::IntegrityManager::kSchemaVersion,
            output.deps, error)) {
      return false;
    }

    ultra::ai::Sha256Hash filesHash{};
    ultra::ai::Sha256Hash symbolsHash{};
    ultra::ai::Sha256Hash depsHash{};
    ultra::ai::Sha256Hash indexHash{};

    if (!ultra::ai::IntegrityManager::computeTableHash(
            filesTablePath, filesHash, error)) {
      return false;
    }
    if (!ultra::ai::IntegrityManager::computeTableHash(
            symbolsTablePath, symbolsHash, error)) {
      return false;
    }
    if (!ultra::ai::IntegrityManager::computeTableHash(
            depsTablePath, depsHash, error)) {
      return false;
    }
    if (!ultra::ai::IntegrityManager::computeIndexHash(
            filesTablePath, symbolsTablePath, depsTablePath, indexHash, error)) {
      return false;
    }

    const ultra::ai::Sha256Hash projectRootHash =
        ultra::ai::IntegrityManager::computeProjectRootHash(output.files);
    const ultra::ai::CoreIndex core = ultra::ai::IntegrityManager::buildCoreIndex(
        true, filesHash, symbolsHash, depsHash, projectRootHash, indexHash);

    if (!ultra::ai::BinaryIndexWriter::writeCoreIndex(corePath, core, error)) {
      return false;
    }

    ultra::ai::RuntimeState state;
    state.core = core;
    state.files = std::move(output.files);
    state.symbols = std::move(output.symbols);
    state.deps = std::move(output.deps);
    state.semanticSymbolDepsByFileId =
        std::move(output.semanticSymbolDepsByFileId);
    state.symbolIndex = std::move(output.symbolIndex);

    stateManager.replaceState(std::move(state));
    {
      const ultra::runtime::GraphSnapshot snap = stateManager.getSnapshot();
      const std::size_t nodeCount = snap.graph ? snap.graph->nodeCount() : 0U;
      stateManager.cognitiveMemory().setGraphScale(nodeCount, 0U);
    }

    std::string persistError;
    if (!stateManager.persistGraphStore(persistError)) {
      error = persistError;
      return false;
    }

    previousState = std::move(priorState);

    const ultra::core::RuntimeStatusSnapshot status =
        stateManager.snapshotStatus(0U);
    payloadOut["message"] = "rebuild_complete";
    payloadOut["runtime_active"] = status.runtimeActive;
    payloadOut["files_indexed"] = status.filesIndexed;
    payloadOut["symbols_indexed"] = status.symbolsIndexed;
    payloadOut["dependencies_indexed"] = status.dependenciesIndexed;
    payloadOut["schema_version"] = status.schemaVersion;
    payloadOut["index_version"] = status.indexVersion;
    return true;
  }

  bool writeAgentContext(nlohmann::json& payloadOut, std::string& error) {
    payloadOut = nlohmann::json::object();
    error.clear();

    std::error_code ec;
    std::filesystem::create_directories(aiDirectory, ec);
    if (ec) {
      error = "Failed to create AI runtime directory: " + aiDirectory.string();
      return false;
    }

    const ultra::ai::RuntimeState state = stateManager.snapshotState();
    if (!ultra::ai::AgentExportBuilder::writeAgentContext(
            agentContextPath, state.files, state.symbols, state.deps, error)) {
      return false;
    }

    std::ifstream input(agentContextPath, std::ios::binary);
    if (!input) {
      error = "Failed to read agent context: " + agentContextPath.string();
      return false;
    }

    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    payloadOut["context_json"] = content;
    payloadOut["context_path"] = agentContextPath.string();
    return true;
  }

  bool computeContextDiff(nlohmann::json& payloadOut, std::string& error) {
    payloadOut = nlohmann::json::object();
    error.clear();

    if (!previousState.has_value()) {
      error = "no_previous_snapshot";
      return false;
    }

    const ultra::ai::RuntimeState current = stateManager.snapshotState();
    const std::uint64_t semanticVersion = stateManager.currentVersion();
    ultra::memory::SemanticMemory* semanticMemory =
        &stateManager.cognitiveMemory().semantic;

    const ultra::runtime::DiffResult diff = ultra::runtime::buildDiffResult(
        *previousState, current, semanticMemory, semanticVersion);

    nlohmann::json added = nlohmann::json::array();
    nlohmann::json removed = nlohmann::json::array();
    nlohmann::json modified = nlohmann::json::array();

    for (const ultra::diff::SymbolDelta& delta : diff.delta.changeObject) {
      switch (delta.changeType) {
        case ultra::types::ChangeType::Added:
          added.push_back(delta.symbolName);
          break;
        case ultra::types::ChangeType::Removed:
          removed.push_back(delta.symbolName);
          break;
        case ultra::types::ChangeType::Modified:
        case ultra::types::ChangeType::Renamed:
          modified.push_back(delta.symbolName);
          break;
        default:
          break;
      }
    }

    payloadOut["added"] = std::move(added);
    payloadOut["removed"] = std::move(removed);
    payloadOut["modified"] = std::move(modified);
    payloadOut["changed"] = diff.changedFiles;

    std::unordered_map<std::uint64_t, std::string> nameById;
    nameById.reserve(current.symbols.size());
    for (const ultra::ai::SymbolRecord& symbol : current.symbols) {
      if (!symbol.name.empty()) {
        nameById[symbol.symbolId] = symbol.name;
      }
    }

    nlohmann::json affected = nlohmann::json::array();
    for (const ultra::runtime::SymbolID symbolId : diff.affectedSymbols) {
      const auto it = nameById.find(symbolId);
      if (it != nameById.end() && !it->second.empty()) {
        affected.push_back(it->second);
      } else {
        affected.push_back(std::to_string(symbolId));
      }
    }
    payloadOut["affected"] = std::move(affected);

    std::ofstream output(contextDiffPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "Failed to write context diff: " + contextDiffPath.string();
      return false;
    }
    output << payloadOut.dump(2);
    if (!output) {
      error = "Failed writing context diff: " + contextDiffPath.string();
      return false;
    }

    return true;
  }

  bool verifyIntegrity(nlohmann::json& payloadOut, std::string& error) {
    payloadOut = nlohmann::json::object();
    error.clear();

    const std::filesystem::path corePath = aiDirectory / "core.idx";
    const std::filesystem::path filesTablePath = aiDirectory / "files.tbl";
    const std::filesystem::path symbolsTablePath = aiDirectory / "symbols.tbl";
    const std::filesystem::path depsTablePath = aiDirectory / "deps.tbl";

    ultra::ai::CoreIndex core;
    if (!ultra::ai::BinaryIndexReader::readCoreIndex(corePath, core, error)) {
      return false;
    }

    std::vector<ultra::ai::FileRecord> files;
    if (!ultra::ai::BinaryIndexReader::readFilesTable(
            filesTablePath, core.schemaVersion, files, error)) {
      return false;
    }

    ultra::ai::Sha256Hash filesHash{};
    ultra::ai::Sha256Hash symbolsHash{};
    ultra::ai::Sha256Hash depsHash{};
    ultra::ai::Sha256Hash indexHash{};

    if (!ultra::ai::IntegrityManager::computeTableHash(
            filesTablePath, filesHash, error)) {
      return false;
    }
    if (!ultra::ai::IntegrityManager::computeTableHash(
            symbolsTablePath, symbolsHash, error)) {
      return false;
    }
    if (!ultra::ai::IntegrityManager::computeTableHash(
            depsTablePath, depsHash, error)) {
      return false;
    }
    if (!ultra::ai::IntegrityManager::computeIndexHash(
            filesTablePath, symbolsTablePath, depsTablePath, indexHash, error)) {
      return false;
    }

    const ultra::ai::Sha256Hash projectRootHash =
        ultra::ai::IntegrityManager::computeProjectRootHash(files);

    if (!ultra::ai::IntegrityManager::verify(core,
                                              filesHash,
                                              symbolsHash,
                                              depsHash,
                                              projectRootHash,
                                              indexHash,
                                              error)) {
      return false;
    }

    payloadOut["integrity_ok"] = true;
    payloadOut["runtime_active"] = core.runtimeActive == 1U;
    payloadOut["schema_version"] = core.schemaVersion;
    payloadOut["index_version"] = core.indexVersion;
    return true;
  }
};


AiRuntimeManager::AiRuntimeManager(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()) {}

bool AiRuntimeManager::requestDaemon(const std::filesystem::path& projectRoot,
                                     const std::string& command,
                                     nlohmann::json& response,
                                     std::string& error) {
  return requestDaemon(projectRoot, command, nlohmann::json::object(), response, error);
}

bool AiRuntimeManager::requestDaemon(const std::filesystem::path& projectRoot,
                                     const std::string& command,
                                     const nlohmann::json& requestPayload,
                                     nlohmann::json& response,
                                     std::string& error) {

  ultra::ipc::UltraIPCClient ipcClient(projectRoot);

  nlohmann::json request;
  request["type"] = command;
  request["payload"] = requestPayload;

  response = ipcClient.sendRequest(request);

  if (!response.is_object()) {
    error = "invalid_response";
    return false;
  }

  if (response.value("status", "") == "error") {
    error = response.value("error", "daemon_error");
    return false;
  }

  return true;
}

int AiRuntimeManager::wakeAi(const bool verbose) {

  if (ultra::runtime::UltraDaemon::isDaemonAlive(projectRoot_)) {
    if (verbose) {
      std::cout << "[UAIR] daemon already running\n";
    }
    return 0;
  }

  // Bug fix 1: current_path() is the working directory, NOT the executable.
  // We must resolve the real binary path so spawnDetached() can exec it.
  const std::filesystem::path execPath = currentExecutablePath();
  if (execPath.empty()) {
    if (verbose) {
      std::cerr << "[UAIR] failed to resolve executable path\n";
    }
    return 1;
  }

  // Bug fix 2: Pass --ultra-daemon so the child process enters runDaemonLoop().
  // This flag must match what daemonChildModeEnabled() checks in argv.
  const std::vector<std::string> daemonArgs{"--ultra-daemon"};

  if (!ultra::runtime::UltraDaemon::wake(projectRoot_, execPath, daemonArgs)) {
    if (verbose) {
      std::cerr << "[UAIR] failed to spawn daemon\n";
    }
    return 1;
  }

  if (verbose) {
    std::cout << "[UAIR] daemon_started\n";
  }

  return 0;
}

int AiRuntimeManager::runDaemonLoop(const bool verbose) {

  ultra::runtime::UltraDaemon daemon(projectRoot_);

  auto dispatcher = std::make_shared<RuntimeDispatcher>(projectRoot_);

  // Do NOT call rebuildIndex here before daemon.run(). The IPC server only
  // starts inside daemon.run() → ipcServer_.start(). Blocking here means the
  // parent's wake() poll (5-second deadline) expires before the server is
  // listening, so the spawn appears to fail. Index is built lazily on the
  // first query via ensureIndex().

  auto runtimeHandler =
      [dispatcher](const std::string& type, const nlohmann::json& payload) -> nlohmann::json {
        return dispatcher->handle(type, payload);
      };

  const bool ok = daemon.run(runtimeHandler);

  if (!ok && verbose) {
    std::cerr << "[UAIR] daemon exited with error\n";
  }

  return ok ? 0 : 1;
}
int AiRuntimeManager::rebuildAi(const bool verbose) {

  nlohmann::json response;
  std::string error;

  if (!requestDaemon(projectRoot_, "rebuild_ai", response, error)) {
    if (verbose) {
      std::cerr << "[UAIR] rebuild request failed: " << error << '\n';
    }
    return 1;
  }

  if (verbose) {
    std::cout << "[UAIR] "
              << response.value("message", "rebuild_enqueued")
              << '\n';
  }

  return response.value("exit_code", 0);
}

int AiRuntimeManager::aiStatus(const bool verbose) {

  nlohmann::json requestPayload = nlohmann::json::object();

  if (verbose) {
    requestPayload["verbose"] = true;
  }

  nlohmann::json response;
  std::string error;

  if (!requestDaemon(projectRoot_, "ai_status", requestPayload, response, error)) {
    if (verbose) {
      std::cerr << "[UAIR] status request failed: " << error << '\n';
    }
    return 1;
  }

  const nlohmann::json payload =
      response.value("payload", nlohmann::json::object());

  (void)printStatusSnapshot(payload, verbose);

  return response.value("exit_code", response.value("ok", false) ? 0 : 1);
}

int AiRuntimeManager::aiVerify(const bool verbose) {

  nlohmann::json response;
  std::string error;

  if (!requestDaemon(projectRoot_, "ai_verify", response, error)) {
    if (verbose) {
      std::cerr << "[UAIR] verify failed: " << error << '\n';
    }
    return 1;
  }

  const nlohmann::json payload =
      response.value("payload", nlohmann::json::object());
  const bool integrityOk = payload.value("integrity_ok", false);

  if (verbose) {
    std::cout << "Integrity OK: " << (integrityOk ? "yes" : "no") << '\n';
    std::cout << "Runtime active: "
              << (payload.value("runtime_active", false) ? "yes" : "no") << '\n';
    std::cout << "Schema version: " << payload.value("schema_version", 0U) << '\n';
    std::cout << "Index version: " << payload.value("index_version", 0U) << '\n';
  }

  return integrityOk ? 0 : 1;
}

bool AiRuntimeManager::contextDiff(nlohmann::json& payloadOut,
                                   std::string& error) {

  if (ultra::runtime::UltraDaemon::isDaemonAlive(projectRoot_)) {

    nlohmann::json response;

    if (!requestDaemon(projectRoot_, "context_diff", response, error)) {
      return false;
    }

    payloadOut = response.value("payload", nlohmann::json::object());
    return true;
  }

  return false;
}

void AiRuntimeManager::silentIncrementalUpdate() {

  nlohmann::json response;
  std::string error;
  nlohmann::json payload;

  payload["verbose"] = false;

  (void)requestDaemon(projectRoot_, "ai_status", payload, response, error);
}

bool AiRuntimeManager::daemonChildModeEnabled() {

#ifdef _WIN32

  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);

  if (argv != nullptr) {

    for (int index = 0; index < argc; ++index) {

      // Bug fix 3: Accept --ultra-daemon (canonical) and --uair-child (legacy alias).
      if (argv[index] != nullptr) {
        const std::wstring arg(argv[index]);
        if (arg == L"--ultra-daemon" || arg == L"--uair-child") {
          ::LocalFree(argv);
          return true;
        }
      }
    }

    ::LocalFree(argv);
  }

  char* buffer = nullptr;
  size_t len = 0U;

  if (_dupenv_s(&buffer, &len, "ULTRA_DAEMON_CHILD") != 0 || buffer == nullptr) {
    return false;
  }

  const std::string value(buffer);
  std::free(buffer);

  return value == "1" || value == "true";

#else

  const char* value = std::getenv("ULTRA_DAEMON_CHILD");

  return value != nullptr &&
         (std::string(value) == "1" || std::string(value) == "true");

#endif
}

}  // namespace ultra::ai
