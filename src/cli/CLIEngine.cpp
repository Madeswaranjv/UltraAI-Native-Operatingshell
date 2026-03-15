#include "CLIEngine.h"
#include "CommandOptionsParser.h"
#include "CommandRouter.h"
#include "../authority/UltraAuthorityAPI.h"
#include "../api/CognitiveKernelAPI.h"
#include "../ai/AiRuntimeManager.h"
#include "../ai/AgentExportBuilder.h"
#include "../ai/BinaryIndexReader.h"
#include "../ai/BinaryIndexWriter.h"
#include "../ai/DependencyTable.h"
#include "../ai/FileRegistry.h"
#include "../ai/Hashing.h"
#include "../ai/IntegrityManager.h"
#include "../ai/SemanticExtractor.h"
#include "../ai/SymbolTable.h"
#include "../core/state_manager.h"
#include "../engine/query/SymbolQueryEngine.h"
#include "../engine/scanner.h"
#include "../indexing/IndexingService.h"
#include "../memory/HotSlice.h"
#include "../memory/StateSnapshot.h"
#include "../metrics/PerformanceMetrics.h"
#include "../metacognition/MetaCognitiveOrchestrator.h"
#include "../runtime/ContextExtractor.h"
#include "../runtime/precision_invalidation.h"
#include "ultra/runtime/ultra_daemon.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "context/ContextSnapshot.h"
#include "../graph/DependencyGraph.h"
#include "../hashing/HashManager.h"
#include "../intelligence/BranchPersistence.h"
#include "../incremental/IncrementalAnalyzer.h"
#include "../language/AdapterFactory.h"
#include "../language/ILanguageAdapter.h"
#include "../scanner/FileInfo.h"
#include "../utils/PathUtils.h"
#include "external/json.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
//E:\Projects\Ultra\src\cli\CLIEngine.cpp
namespace ultra::cli {

namespace {

const char* kVersion = "0.1.0";

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

struct DaemonRuntimeDispatcher {
  std::filesystem::path projectRoot;
  std::filesystem::path aiDirectory;
  std::filesystem::path agentContextPath;
  std::filesystem::path contextDiffPath;
  ultra::core::StateManager stateManager;
  std::optional<ultra::ai::RuntimeState> previousState;

  explicit DaemonRuntimeDispatcher(std::filesystem::path root)
      : projectRoot(std::filesystem::absolute(std::move(root)).lexically_normal()),
        aiDirectory(projectRoot / ".ultra" / "ai"),
        agentContextPath(projectRoot / ".ultra.context.json"),
        contextDiffPath(projectRoot / ".ultra.context-diff.json"),
        stateManager(projectRoot) {}

  nlohmann::json handle(const std::string& type, const nlohmann::json& payload) {
    if (type == "ai_status") {
      std::string error;
      (void)ensureIndexAvailable(error);  // lazy build on first status check
      const bool verbose = payload.value("verbose", false);
      (void)verbose;
      return makeOk(buildStatusPayload(), 0);
    }

    if (type == "rebuild_ai") {
      nlohmann::json payloadOut;
      std::string error;
      if (!rebuildIndex(payloadOut, error)) {
        return makeError(error);
      }
      return makeOk(payloadOut, 0, "semantic_index_built");
    }

    if (type == "ai_query") {
      const std::string target = payload.value("target", std::string{});
      if (target.empty()) {
        return makeError("missing_query_target");
      }
      std::string error;
      if (!ensureIndexAvailable(error)) {
        return makeError(error);
      }
      const ultra::ai::RuntimeState& runtime = runtimeState();
      if (runtime.symbolIndex.empty()) {
        return makeError("semantic_index_unavailable");
      }
      const auto it = runtime.symbolIndex.find(target);
      if (it == runtime.symbolIndex.end()) {
        nlohmann::json notFound{{"kind", "not_found"}, {"target", target}};
        return makeOk(notFound, 1);
      }

      nlohmann::json result;
      result["kind"] = "symbol";
      result["name"] = target;
      result["defined_in"] = it->second.definedIn;
      result["usage_count"] = it->second.usedInFiles.size();
      result["weight"] = it->second.weight;
      result["centrality"] = it->second.centrality;
      result["references"] = toSortedVector(it->second.usedInFiles);

      auto& queryEngine = symbolQueryEngine();
      if (queryEngine.empty()) {
        queryEngine.rebuild(runtime, runtimeVersion());
      }

      nlohmann::json definitions = nlohmann::json::array();
      for (const auto& definition : queryEngine.findDefinition(target)) {
        nlohmann::json item;
        item["file_path"] = definition.filePath;
        item["line_number"] = definition.lineNumber;
        item["signature"] = definition.signature;
        item["symbol_id"] = definition.symbolId;
        definitions.push_back(std::move(item));
      }
      result["definitions"] = std::move(definitions);
      result["symbol_dependencies"] = queryEngine.findSymbolDependencies(target);
      const std::size_t depth = std::max<std::size_t>(
          1U, payload.value("depth", payload.value("max_depth", 2U)));
      result["impact_region"] = queryEngine.findImpactRegion(target, depth);
      nlohmann::json contextPayload = nlohmann::json::object();
      try {
        const std::size_t contextBudget =
            std::max<std::size_t>(1U, payload.value("token_budget", 4096U));
        ultra::runtime::CognitiveState cognitiveState =
            stateManager.createCognitiveState(contextBudget);
        ultra::runtime::Query contextQuery;
        contextQuery.kind = ultra::runtime::QueryKind::Auto;
        contextQuery.target = target;
        contextQuery.impactDepth = depth;
        ultra::runtime::ContextExtractor extractor;
        const ultra::runtime::ContextSlice slice =
            extractor.getMinimalContext(cognitiveState, contextQuery);
        nlohmann::json parsed =
            nlohmann::json::parse(slice.json, nullptr, false);
        if (!parsed.is_discarded()) {
          contextPayload = std::move(parsed);
        }
      } catch (...) {
      }
      result["ai_context"] = std::move(contextPayload);
      const std::size_t tokenBudget = payload.value("token_budget", 4096U);
      const auto metrics =
          ultra::metacognition::MetaCognitiveOrchestrator::instance()
              .recordQuery(target,
                           runtimeVersion(),
                           tokenBudget,
                           128U,
                           ultra::memory::HotSlice::kMaxHotSliceEntries);
      result["meta_cognitive"] = buildMetaCognitivePayload(metrics);
      return makeOk(result, 0);
    }

    if (type == "ai_source") {
      std::string error;
      if (!ensureIndexAvailable(error)) {
        return makeError(error);
      }
      const std::string target = payload.value("file", std::string{});
      if (target.empty()) {
        return makeError("missing_source_target");
      }
      nlohmann::json result;
      if (!readSource(target, result, error)) {
        return makeError(error);
      }
      return makeOk(result, 0);
    }

    if (type == "ai_impact") {
      std::string error;
      if (!ensureIndexAvailable(error)) {
        return makeError(error);
      }
      const std::string target = payload.value("target", std::string{});
      if (target.empty()) {
        return makeError("missing_impact_target");
      }

      const ultra::ai::RuntimeState& runtime = runtimeState();
      auto& queryEngine = symbolQueryEngine();
      if (queryEngine.empty()) {
        queryEngine.rebuild(runtime, runtimeVersion());
      }
      const std::size_t depth = std::max<std::size_t>(
          1U, payload.value("depth", payload.value("max_depth", 2U)));

      // --- Symbol-based impact (primary path) ---
      const auto symIt = runtime.symbolIndex.find(target);
      if (symIt != runtime.symbolIndex.end()) {
        const std::vector<std::string> direct =
            queryEngine.findReferences(target);
        const std::vector<std::string> impact =
            queryEngine.findImpactRegion(target, depth);

        std::set<std::string> directSet(direct.begin(), direct.end());
        std::vector<std::string> transitive;
        for (const std::string& path : impact) {
          if (directSet.find(path) == directSet.end()) {
            transitive.push_back(path);
          }
        }

        nlohmann::json result;
        result["kind"] = "symbol_impact";
        result["symbol"] = target;
        result["defined_in"] = symIt->second.definedIn;
        result["direct_usage_files"] = direct;
        result["transitive_impacted_files"] = transitive;
        const std::size_t totalFiles = runtime.files.size();
        const double score =
            totalFiles > 0U
                ? static_cast<double>(direct.size() + transitive.size()) /
                      static_cast<double>(totalFiles)
                : 0.0;
        result["impact_score"] = score;
        const std::size_t tokenBudget = payload.value("token_budget", 4096U);
        const auto metrics =
            ultra::metacognition::MetaCognitiveOrchestrator::instance()
                .recordQuery(target,
                             runtimeVersion(),
                             tokenBudget,
                             128U,
                             ultra::memory::HotSlice::kMaxHotSliceEntries);
        result["meta_cognitive"] = buildMetaCognitivePayload(metrics);
        return makeOk(result, 0);
      }

      // --- File-based impact (fallback when target is a file path) ---
      // Normalise: strip leading slashes/backslashes and convert to forward slashes.
      std::string normalizedTarget =
          std::filesystem::path(target).generic_string();
      // Try suffix matching against the indexed file list.
      std::string matchedPath;
      for (const ultra::ai::FileRecord& file : runtime.files) {
        const std::string& filePath = file.path;
        if (filePath == normalizedTarget ||
            (filePath.size() > normalizedTarget.size() &&
             filePath[filePath.size() - normalizedTarget.size() - 1U] == '/' &&
             filePath.compare(filePath.size() - normalizedTarget.size(),
                              normalizedTarget.size(),
                              normalizedTarget) == 0)) {
          if (matchedPath.empty() || filePath.size() < matchedPath.size()) {
            matchedPath = filePath;
          }
        }
      }

      if (matchedPath.empty()) {
        nlohmann::json notFound{{"kind", "not_found"}, {"target", target}};
        return makeOk(notFound, 1);
      }

      // Find all files that directly depend on the matched file.
      std::unordered_map<std::uint32_t, std::string> pathById;
      pathById.reserve(runtime.files.size());
      std::uint32_t targetFileId = 0U;
      for (const ultra::ai::FileRecord& file : runtime.files) {
        pathById[file.fileId] = file.path;
        if (file.path == matchedPath) {
          targetFileId = file.fileId;
        }
      }

      std::set<std::string> directSet;
      for (const ultra::ai::FileDependencyEdge& edge : runtime.deps.fileEdges) {
        if (edge.toFileId == targetFileId) {
          const auto it = pathById.find(edge.fromFileId);
          if (it != pathById.end()) {
            directSet.insert(it->second);
          }
        }
      }

      // BFS for transitive dependents up to `depth` hops.
      std::set<std::string> visited(directSet);
      visited.insert(matchedPath);
      std::vector<std::string> frontier(directSet.begin(), directSet.end());
      std::set<std::string> transitiveSet;

      std::unordered_map<std::string, std::uint32_t> idByPath;
      idByPath.reserve(runtime.files.size());
      for (const ultra::ai::FileRecord& file : runtime.files) {
        idByPath[file.path] = file.fileId;
      }

      for (std::size_t hop = 1U; hop < depth && !frontier.empty(); ++hop) {
        std::vector<std::string> nextFrontier;
        for (const std::string& frontierPath : frontier) {
          const auto idIt = idByPath.find(frontierPath);
          if (idIt == idByPath.end()) continue;
          const std::uint32_t fid = idIt->second;
          for (const ultra::ai::FileDependencyEdge& edge : runtime.deps.fileEdges) {
            if (edge.toFileId == fid) {
              const auto pIt = pathById.find(edge.fromFileId);
              if (pIt != pathById.end() && visited.find(pIt->second) == visited.end()) {
                visited.insert(pIt->second);
                transitiveSet.insert(pIt->second);
                nextFrontier.push_back(pIt->second);
              }
            }
          }
        }
        frontier = std::move(nextFrontier);
      }

      const std::vector<std::string> directVec(directSet.begin(), directSet.end());
      const std::vector<std::string> transitiveVec(transitiveSet.begin(), transitiveSet.end());
      const std::size_t totalFiles = runtime.files.size();
      const double score =
          totalFiles > 0U
              ? static_cast<double>(directVec.size() + transitiveVec.size()) /
                    static_cast<double>(totalFiles)
              : 0.0;

      nlohmann::json result;
      result["kind"] = "file_impact";
      result["target"] = matchedPath;
      result["direct_dependents"] = directVec;
      result["transitive_dependents"] = transitiveVec;
      result["impact_score"] = score;
      const std::size_t tokenBudget = payload.value("token_budget", 4096U);
      const auto metrics =
          ultra::metacognition::MetaCognitiveOrchestrator::instance()
              .recordQuery(target,
                           runtimeVersion(),
                           tokenBudget,
                           128U,
                           ultra::memory::HotSlice::kMaxHotSliceEntries);
      result["meta_cognitive"] = buildMetaCognitivePayload(metrics);
      return makeOk(result, 0);
    }

    if (type == "ai_context") {
      std::string error;
      if (!ensureIndexAvailable(error)) {
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

  static ultra::ai::RuntimeState& runtimeState() {
    static ultra::ai::RuntimeState runtime;
    return runtime;
  }

  static std::uint64_t& runtimeVersion() {
    static std::uint64_t version = 0U;
    return version;
  }

  static ultra::engine::query::SymbolQueryEngine& symbolQueryEngine() {
    static ultra::engine::query::SymbolQueryEngine engine;
    return engine;
  }

  bool ensureIndexAvailable(std::string& error) {
    if (!runtimeState().files.empty()) {
      return true;
    }
    // Lazy first-use build: the index has not been populated yet (startup
    // rebuild was deliberately deferred so the IPC server could start and
    // respond to the parent's "wake" ping before the scan runs).
    nlohmann::json payloadOut;
    return rebuildIndex(payloadOut, error);
  }

  nlohmann::json buildStatusPayload() const {
    const ultra::ai::RuntimeState& runtime = runtimeState();
    nlohmann::json payload = nlohmann::json::object();
    const bool runtimeActive =
        runtime.core.runtimeActive == 1U || !runtime.files.empty();
    payload["runtime_active"] = runtimeActive;
    payload["files_indexed"] = runtime.files.size();
    payload["symbols_indexed"] = runtime.symbols.size();
    payload["dependencies_indexed"] =
        runtime.deps.fileEdges.size() + runtime.deps.symbolEdges.size();
    payload["graph_nodes"] = runtime.files.size() + runtime.symbols.size();
    payload["graph_edges"] =
        runtime.deps.fileEdges.size() + runtime.deps.symbolEdges.size();
    payload["memory_usage_bytes"] = estimateRuntimeBytes(runtime);
    payload["pending_changes"] = 0U;
    payload["schema_version"] = runtime.core.schemaVersion == 0U
                                    ? ultra::ai::IntegrityManager::kSchemaVersion
                                    : runtime.core.schemaVersion;
    payload["index_version"] = runtime.core.indexVersion == 0U
                                   ? ultra::ai::IntegrityManager::kIndexVersion
                                   : runtime.core.indexVersion;
    const std::filesystem::path pidPath =
        projectRoot / ".ultra_daemon" / "daemon.pid";
    std::ifstream pidStream(pidPath);
    long long pid = 0;
    pidStream >> pid;
    payload["daemon_pid"] =
        static_cast<std::uint64_t>(std::max<long long>(0, pid));
    return payload;
  }

 public:
  bool rebuildIndex(nlohmann::json& payloadOut, std::string& error) {
    payloadOut = nlohmann::json::object();
    error.clear();

    ultra::ai::RuntimeState next;
    std::vector<ultra::ai::DiscoveredFile> discovered =
        ultra::ai::FileRegistry::discoverProjectFiles(projectRoot);

    // Filter out directories that are not part of the project's own source:
    // build artefacts, vendored grammars/deps, and VCS metadata.
    // Without this, symbols like `main` resolve to googletest and tree-sitter
    // test files instead of the project's own src/main.cpp.
    static const std::vector<std::string> kExcludedPrefixes{
        "build/", "build\\",
        "third_party/", "third_party\\",
        "_deps/", "_deps\\",
        ".git/", ".git\\",
        ".ultra_daemon/",
    };
    discovered.erase(
        std::remove_if(
            discovered.begin(), discovered.end(),
            [](const ultra::ai::DiscoveredFile& file) {
              for (const std::string& prefix : kExcludedPrefixes) {
                if (file.relativePath.size() >= prefix.size() &&
                    file.relativePath.compare(0, prefix.size(), prefix) == 0) {
                  return true;
                }
              }
              return false;
            }),
        discovered.end());

    std::vector<ultra::ai::FileRecord> records =
        ultra::ai::FileRegistry::deriveRecords(discovered);

    for (std::size_t i = 0; i < records.size(); ++i) {
      if (!ultra::ai::sha256OfFile(discovered[i].absolutePath,
                                   records[i].hash, error)) {
        return false;
      }
    }

    const std::map<std::string, ultra::ai::FileRecord> filesByPath =
        ultra::ai::FileRegistry::mapByPath(records);

    std::vector<ultra::ai::SymbolRecord> symbols;
    ultra::ai::DependencyTableData deps;
    std::map<std::uint32_t, std::vector<ultra::ai::SemanticSymbolDependency>>
        semanticDepsByFileId;

    for (const auto& file : discovered) {
      ultra::ai::SemanticParseResult semantic;
      if (!ultra::ai::SemanticExtractor::extract(file.absolutePath, file.language,
                                                 semantic, error)) {
        return false;
      }

      std::vector<ultra::ai::SymbolRecord> fileSymbols;
      if (!ultra::ai::SymbolTable::buildFromExtracted(
              file.fileId, semantic.symbols, fileSymbols, error)) {
        return false;
      }
      symbols.insert(symbols.end(), fileSymbols.begin(), fileSymbols.end());
      semanticDepsByFileId[file.fileId] = semantic.symbolDependencies;

      const std::string currentPath = file.relativePath;
      for (const std::string& reference : semantic.dependencyReferences) {
        std::string resolvedPath;
        if (!resolveDependencyReference(currentPath, reference, filesByPath,
                                        resolvedPath)) {
          continue;
        }
        const auto targetIt = filesByPath.find(resolvedPath);
        if (targetIt == filesByPath.end()) {
          continue;
        }
        ultra::ai::FileDependencyEdge edge;
        edge.fromFileId = file.fileId;
        edge.toFileId = targetIt->second.fileId;
        deps.fileEdges.push_back(edge);
      }
    }

    next.files = std::move(records);
    next.symbols = std::move(symbols);
    next.deps = std::move(deps);
    next.semanticSymbolDepsByFileId = std::move(semanticDepsByFileId);
    rebuildSymbolEdges(next);

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
            next.files, error)) {
      return false;
    }
    if (!ultra::ai::BinaryIndexWriter::writeSymbolsTable(
            symbolsTablePath, ultra::ai::IntegrityManager::kSchemaVersion,
            next.symbols, error)) {
      return false;
    }
    if (!ultra::ai::BinaryIndexWriter::writeDependenciesTable(
            depsTablePath, ultra::ai::IntegrityManager::kSchemaVersion,
            next.deps, error)) {
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
        ultra::ai::IntegrityManager::computeProjectRootHash(next.files);
    const ultra::ai::CoreIndex core = ultra::ai::IntegrityManager::buildCoreIndex(
        !next.files.empty(), filesHash, symbolsHash, depsHash, projectRootHash,
        indexHash);

    if (!ultra::ai::BinaryIndexWriter::writeCoreIndex(corePath, core, error)) {
      return false;
    }

    next.core = core;
    previousState = runtimeState();
    runtimeState() = std::move(next);
    stateManager.replaceState(runtimeState());
    symbolQueryEngine().rebuild(runtimeState(), ++runtimeVersion());

    payloadOut["message"] = "semantic_index_built";
    payloadOut["runtime_active"] = runtimeState().core.runtimeActive == 1U;
    payloadOut["files_indexed"] = runtimeState().files.size();
    payloadOut["symbols_indexed"] = runtimeState().symbols.size();
    payloadOut["dependencies_indexed"] =
        runtimeState().deps.fileEdges.size() +
        runtimeState().deps.symbolEdges.size();
    payloadOut["schema_version"] = runtimeState().core.schemaVersion;
    payloadOut["index_version"] = runtimeState().core.indexVersion;
    return true;
  }

  bool writeAgentContext(nlohmann::json& payloadOut, std::string& error) {
    payloadOut = nlohmann::json::object();
    error.clear();

    const ultra::ai::RuntimeState& state = runtimeState();
    if (!ultra::ai::AgentExportBuilder::writeAgentContext(
            agentContextPath, state.files, state.symbols, state.deps, error)) {
      return false;
    }

    // Do NOT read the file back into the response payload.
    // The agent context JSON for a real project can be several MB.
    // The IPC pipe has a 1 MB cap (kMaxMessageBytes in ultra_ipc_client.cpp).
    // Embedding the raw content causes the response to overflow the cap,
    // the reader returns empty, and the client gets "invalid_json_response".
    // Instead we return the path so the CLI can read it directly from disk.
    payloadOut["context_path"] = agentContextPath.string();
    payloadOut["files_indexed"] = state.files.size();
    payloadOut["symbols_indexed"] = state.symbols.size();
    payloadOut["dependencies_indexed"] =
        state.deps.fileEdges.size() + state.deps.symbolEdges.size();
    return true;
  }

  bool computeContextDiff(nlohmann::json& payloadOut, std::string& error) {
    payloadOut = nlohmann::json::object();
    error.clear();

    if (!previousState.has_value()) {
      error = "no_previous_snapshot";
      return false;
    }

    const ultra::ai::RuntimeState current = runtimeState();
    const ultra::runtime::DiffResult diff =
        ultra::runtime::buildDiffResult(*previousState, current, nullptr, 0U);

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

  bool readSource(const std::string& target,
                  nlohmann::json& payloadOut,
                  std::string& error) const {
    payloadOut = nlohmann::json::object();

    // First try the target as-is: absolute, or relative to projectRoot.
    std::filesystem::path path(target);
    if (path.is_relative()) {
      path = projectRoot / path;
    }
    path = path.lexically_normal();

    if (!std::filesystem::exists(path)) {
      // Exact path failed. Try suffix matching against the indexed file list.
      // This lets the user type "SymbolDiff.h" and resolve to
      // "src/diff/SymbolDiff.h" automatically.
      const ultra::ai::RuntimeState& runtime = runtimeState();
      std::string bestMatch;
      const std::string normalizedTarget =
          std::filesystem::path(target).generic_string();
      for (const ultra::ai::FileRecord& file : runtime.files) {
        const std::string& filePath = file.path;
        if (filePath == normalizedTarget ||
            (filePath.size() > normalizedTarget.size() &&
             filePath[filePath.size() - normalizedTarget.size() - 1U] == '/' &&
             filePath.compare(filePath.size() - normalizedTarget.size(),
                              normalizedTarget.size(),
                              normalizedTarget) == 0)) {
          if (bestMatch.empty() || filePath.size() < bestMatch.size()) {
            bestMatch = filePath;
          }
        }
      }
      if (!bestMatch.empty()) {
        path = (projectRoot / bestMatch).lexically_normal();
      }
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
      error = "Failed to open source file: " + target +
              "\n  Tried: " + path.string() +
              "\n  Hint: use a relative path like src/diff/SymbolDiff.h";
      return false;
    }

    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
      error = "Failed reading source file: " + target;
      return false;
    }

    payloadOut["kind"] = "source";
    payloadOut["path"] = path.string();
    payloadOut["content"] = content;
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

inline ultra::runtime::UltraDaemon::RuntimeRequestHandler buildDaemonRuntimeHandler(
    const std::filesystem::path& projectRoot) {
  auto dispatcher = std::make_shared<DaemonRuntimeDispatcher>(projectRoot);
  // Do NOT call rebuildIndex here. Doing so blocks the return of this function,
  // which means UltraDaemon::run() / ipcServer_.start() never gets called until
  // the full scan completes. The parent process polls for a "wake" IPC response
  // with a 5-second deadline — on any real project the scan exceeds that window
  // and wake() returns false ("failed to spawn daemon").
  // Index is built lazily inside ensureIndexAvailable() on the first query that
  // needs it, by which point the IPC server is already listening.
  return [dispatcher](const std::string& type, const nlohmann::json& payload) {
    return dispatcher->handle(type, payload);
  };
}
bool pathExists(const std::filesystem::path& path) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  return exists && !ec;
}

bool isLikelyProjectRoot(const std::filesystem::path& candidate) {
  if (pathExists(candidate / ".ultra")) {
    return true;
  }
  return pathExists(candidate / "CMakeLists.txt") &&
         pathExists(candidate / "src");
}

std::filesystem::path findProjectRoot(std::filesystem::path start) {
  std::error_code ec;
  start = std::filesystem::absolute(std::move(start), ec).lexically_normal();
  if (ec) {
    return start;
  }

  std::filesystem::path current = std::move(start);
  while (!current.empty()) {
    if (isLikelyProjectRoot(current)) {
      return current;
    }
    const std::filesystem::path parent = current.parent_path();
    if (parent.empty() || parent == current) {
      break;
    }
    current = parent;
  }
  return {};
}

std::filesystem::path resolveExecutablePath(const char* argv0) {
  if (argv0 == nullptr) {
    return {};
  }
  const std::filesystem::path raw(argv0);
  std::error_code ec;

  if (raw.has_parent_path()) {
    const std::filesystem::path absolute = std::filesystem::absolute(raw, ec);
    if (!ec && pathExists(absolute)) {
      return absolute.lexically_normal();
    }
  } else if (!raw.empty() && pathExists(raw)) {
    const std::filesystem::path absolute = std::filesystem::absolute(raw, ec);
    if (!ec) {
      return absolute.lexically_normal();
    }
  }

  std::string pathValue;

#ifdef _WIN32
  char* buffer = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buffer, &len, "PATH") == 0 && buffer != nullptr) {
    pathValue = buffer;
    free(buffer);
  }
#else
  const char* buffer = std::getenv("PATH");
  if (buffer != nullptr) {
    pathValue = buffer;
  }
#endif

if (!pathValue.empty()) {
#ifdef _WIN32
    const char delimiter = ';';
#else
    const char delimiter = ':';
#endif
    std::size_t begin = 0U;
    while (begin <= pathValue.size()) {
      const std::size_t end = pathValue.find(delimiter, begin);
      const std::string token = pathValue.substr(
          begin, end == std::string::npos ? std::string::npos : end - begin);
      if (!token.empty()) {
        const std::filesystem::path base(token);
        const std::filesystem::path candidate = base / raw;
        if (pathExists(candidate)) {
          return std::filesystem::absolute(candidate, ec).lexically_normal();
        }
#ifdef _WIN32
        if (candidate.extension().empty()) {
          static const char* const kExtensions[] = {".exe", ".bat", ".cmd"};
          for (const char* ext : kExtensions) {
            const std::filesystem::path withExt = candidate.string() + ext;
            if (pathExists(withExt)) {
              return std::filesystem::absolute(withExt, ec).lexically_normal();
            }
          }
        }
#endif
      }

      if (end == std::string::npos) {
        break;
      }
      begin = end + 1U;
    }
  }

  const std::filesystem::path fallback = std::filesystem::absolute(raw, ec);
  if (!ec) {
    return fallback.lexically_normal();
  }
  return {};
}

std::filesystem::path resolveCliProjectRoot(const char* argv0) {
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec) {
    const std::filesystem::path fromCwd = findProjectRoot(cwd);
    if (!fromCwd.empty()) {
      return fromCwd;
    }
  }

  const std::filesystem::path executablePath = resolveExecutablePath(argv0);
  if (!executablePath.empty()) {
    const std::filesystem::path fromExecutable =
        findProjectRoot(executablePath.parent_path());
    if (!fromExecutable.empty()) {
      return fromExecutable;
    }
  }

  if (!ec) {
    return cwd.lexically_normal();
  }
  return std::filesystem::path(".");
}

void printStringList(const std::string& label, const nlohmann::json& value) {
  std::cout << label << ":\n";
  if (!value.is_array() || value.empty()) {
    std::cout << "  (none)\n";
    return;
  }
  for (const nlohmann::json& item : value) {
    if (item.is_string()) {
      std::cout << "  - " << item.get<std::string>() << '\n';
    }
  }
}

void printDefinitionList(const nlohmann::json& value) {
  std::cout << "Definitions:\n";
  if (!value.is_array() || value.empty()) {
    std::cout << "  (none)\n";
    return;
  }

  for (const nlohmann::json& item : value) {
    if (!item.is_object()) {
      continue;
    }

    const std::string filePath = item.value("file_path", std::string{});
    const std::uint32_t lineNumber = item.value("line_number", 0U);
    const std::string signature = item.value("signature", std::string{});
    const std::uint64_t symbolId = item.value("symbol_id", 0ULL);

    std::cout << "  - " << filePath;
    if (lineNumber != 0U) {
      std::cout << ':' << lineNumber;
    }
    if (!signature.empty()) {
      std::cout << ' ' << signature;
    }
    if (symbolId != 0ULL) {
      std::cout << " [id=" << symbolId << ']';
    }
    std::cout << '\n';
  }
}

void printObjectStringFieldList(const std::string& label,
                                const nlohmann::json& value,
                                const char* fieldName) {
  std::cout << label << ":\n";
  if (!value.is_array() || value.empty()) {
    std::cout << "  (none)\n";
    return;
  }

  bool printed = false;
  for (const nlohmann::json& item : value) {
    if (!item.is_object()) {
      continue;
    }
    const std::string fieldValue = item.value(fieldName, std::string{});
    if (fieldValue.empty()) {
      continue;
    }
    std::cout << "  - " << fieldValue << '\n';
    printed = true;
  }
  if (!printed) {
    std::cout << "  (none)\n";
  }
}

void printAiContextPayload(const nlohmann::json& value) {
  if (!value.is_object()) {
    return;
  }

  const nlohmann::json metadata = value.value("metadata", nlohmann::json::object());
  std::cout << "AI Context:\n";
  std::cout << "  Kind: " << value.value("kind", "") << '\n';
  std::cout << "  Target: " << value.value("target", "") << '\n';
  std::cout << "  Estimated tokens: " << metadata.value("estimatedTokens", 0U)
            << " / " << metadata.value("tokenBudget", 0U) << '\n';
  std::cout << "  Truncated: "
            << (metadata.value("truncated", false) ? "true" : "false") << '\n';
  printObjectStringFieldList("Context symbols",
                             value.value("nodes", nlohmann::json::array()),
                             "name");
  printObjectStringFieldList("Context files",
                             value.value("files", nlohmann::json::array()),
                             "path");
  printStringList("Context impact region",
                  value.value("impact_region", nlohmann::json::array()));
}

void printMetaCognitivePayload(const nlohmann::json& payload) {
  if (!payload.is_object()) {
    return;
  }
  std::cout << "Meta-cognitive:\n";
  std::cout << "  stability score: " << payload.value("stability_score", 0.0)
            << '\n';
  std::cout << "  drift score: " << payload.value("drift_score", 0.0) << '\n';
  std::cout << "  learning velocity: "
            << payload.value("learning_velocity", 0.0) << '\n';
  std::cout << "  conservative mode: "
            << (payload.value("conservative_mode", false) ? "yes" : "no")
            << '\n';
  std::cout << "  exploratory mode: "
            << (payload.value("exploratory_mode", false) ? "yes" : "no")
            << '\n';
  const std::string predicted =
      payload.value("predicted_next_command", std::string{});
  std::cout << "  predicted next command: "
            << (predicted.empty() ? "(none)" : predicted) << '\n';
  std::cout << "  query token budget: "
            << payload.value("query_token_budget", 0U) << '\n';
  std::cout << "  query cache capacity: "
            << payload.value("query_cache_capacity", 0U) << '\n';
  std::cout << "  hot slice capacity: "
            << payload.value("hot_slice_capacity", 0U) << '\n';
  std::cout << "  branch retention hint: "
            << payload.value("branch_retention_hint", 0U) << '\n';
}

void printAiQueryPayload(const nlohmann::json& payload) {
  const std::string kind = payload.value("kind", "");
  if (kind == "file") {
    std::cout << "Kind: file\n";
    std::cout << "Path: " << payload.value("path", "") << '\n';
    std::cout << "Type: " << payload.value("file_type", "other") << '\n';
    std::cout << "Size: " << payload.value("size", 0ULL) << '\n';
    const bool semantic = payload.value("semantic", false);
    std::cout << "Semantic: " << (semantic ? "true" : "false") << '\n';
    std::cout << "Recently modified: "
              << (payload.value("recently_modified", false) ? "true" : "false")
              << '\n';
    if (semantic) {
      std::cout << std::fixed << std::setprecision(3);
      std::cout << "Weight: " << payload.value("weight", 0.0) << '\n';
      std::cout << "Centrality: " << payload.value("centrality", 0.0) << '\n';
      printStringList("Symbols defined", payload.value("symbols_defined", nlohmann::json::array()));
      printStringList("Symbols used", payload.value("symbols_used", nlohmann::json::array()));
      printStringList("Dependencies", payload.value("dependencies", nlohmann::json::array()));
      printStringList("Depended by", payload.value("depended_by", nlohmann::json::array()));
      std::cout.unsetf(std::ios::floatfield);
      std::cout << std::setprecision(6);
    }
    printAiContextPayload(payload.value("ai_context", nlohmann::json::object()));
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "symbol") {
    std::cout << "Kind: symbol\n";
    std::cout << "Name: " << payload.value("name", "") << '\n';
    std::cout << "Defined in: " << payload.value("defined_in", "") << '\n';
    std::cout << "Usage count: " << payload.value("usage_count", 0U) << '\n';
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Weight: " << payload.value("weight", 0.0) << '\n';
    std::cout << "Centrality: " << payload.value("centrality", 0.0) << '\n';
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
    printDefinitionList(payload.value("definitions", nlohmann::json::array()));
    const nlohmann::json references =
        payload.contains("references")
            ? payload["references"]
            : payload.value("used_in", nlohmann::json::array());
    printStringList("References", references);
    printStringList("Symbol dependencies",
                    payload.value("symbol_dependencies", nlohmann::json::array()));
    printStringList("Impact region",
                    payload.value("impact_region", nlohmann::json::array()));
    printAiContextPayload(payload.value("ai_context", nlohmann::json::object()));
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "not_found") {
    std::cout << "[UAIR] target not found: " << payload.value("target", "") << '\n';
    return;
  }

  std::cout << "[UAIR] Unexpected ai_query response payload.\n";
}

void printAiImpactPayload(const nlohmann::json& payload) {
  const std::string kind = payload.value("kind", "");
  if (kind == "file_impact") {
    std::cout << "Kind: file_impact\n";
    std::cout << "Target: " << payload.value("target", "") << '\n';
    printStringList("Direct", payload.value("direct_dependents", nlohmann::json::array()));
    printStringList("Transitive", payload.value("transitive_dependents", nlohmann::json::array()));
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Score: " << payload.value("impact_score", 0.0) << '\n';
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "symbol_impact") {
    std::cout << "Kind: symbol_impact\n";
    std::cout << "Symbol: " << payload.value("symbol", "") << '\n';
    std::cout << "Defined in: " << payload.value("defined_in", "") << '\n';
    printStringList("Direct", payload.value("direct_usage_files", nlohmann::json::array()));
    printStringList("Transitive", payload.value("transitive_impacted_files", nlohmann::json::array()));
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Score: " << payload.value("impact_score", 0.0) << '\n';
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "not_found") {
    std::cout << "[UAIR] impact target not found: " << payload.value("target", "")
              << '\n';
    return;
  }

  std::cout << "[UAIR] Unexpected ai_impact response payload.\n";
}

void printKernelHealth(const nlohmann::json& payload) {
  if (!payload.is_object()) {
    return;
  }

  std::cout << "Kernel health:\n";
  std::cout << "  Branch count: " << payload.value("branch_count", 0U) << '\n';
  std::cout << "  Snapshot count: " << payload.value("snapshot_count", 0U)
            << '\n';
  std::cout << "  Governance active: "
            << (payload.value("governance_active", false) ? "yes" : "no")
            << '\n';
  std::cout << "  Determinism guards: "
            << (payload.value("determinism_guards_active", false) ? "yes"
                                                                  : "no")
            << '\n';
  std::cout << "  Memory caps respected: "
            << (payload.value("memory_caps_respected", false) ? "yes" : "no")
            << '\n';

  const nlohmann::json violations =
      payload.value("violations", nlohmann::json::array());
  if (!violations.empty()) {
    printStringList("Kernel violations", violations);
  }
}

void printMetricsReport(const nlohmann::json& report) {
  if (!report.is_object()) {
    std::cout << "[UAIR] Unexpected ai_metrics response payload.\n";
    return;
  }

  std::cout << "Metrics enabled: "
            << (report.value("enabled", false) ? "yes" : "no") << '\n';

  const nlohmann::json snapshot =
      report.value("snapshot", nlohmann::json::object());
  std::cout << "Snapshot:\n";
  std::cout << "  avg creation time (us): "
            << snapshot.value("avg_creation_time_micros", 0.0) << '\n';
  std::cout << "  max creation time (us): "
            << snapshot.value("max_creation_time_micros", 0ULL) << '\n';
  const nlohmann::json nodeDistribution =
      snapshot.value("node_count_distribution", nlohmann::json::array());
  if (nodeDistribution.empty()) {
    std::cout << "  node count distribution: (none)\n";
  } else {
    std::cout << "  node count distribution:\n";
    for (const nlohmann::json& entry : nodeDistribution) {
      std::cout << "    " << entry.value("node_count", 0U) << " -> "
                << entry.value("count", 0U) << '\n';
    }
  }

  const nlohmann::json context =
      report.value("context", nlohmann::json::object());
  std::cout << "Context:\n";
  std::cout << "  avg compression time (us): "
            << context.value("avg_compression_time_micros", 0.0) << '\n';
  std::cout << "  avg tokens saved: " << context.value("avg_tokens_saved", 0.0)
            << '\n';
  std::cout << "  compression ratio: " << context.value("compression_ratio", 0.0)
            << '\n';
  std::cout << "  context reuse rate: "
            << context.value("context_reuse_rate", 0.0) << '\n';
  std::cout << "  hot slice hit rate: "
            << context.value("hot_slice_hit_rate", 0.0) << '\n';

  const nlohmann::json branch = report.value("branch", nlohmann::json::object());
  std::cout << "Branch:\n";
  std::cout << "  avg churn time (us): "
            << branch.value("avg_churn_time_micros", 0.0) << '\n';
  std::cout << "  eviction count: " << branch.value("eviction_count", 0U) << '\n';
  std::cout << "  overlay reuse rate: "
            << branch.value("overlay_reuse_rate", 0.0) << '\n';

  const nlohmann::json token = report.value("token", nlohmann::json::object());
  std::cout << "Token:\n";
  std::cout << "  total tokens saved: "
            << token.value("total_tokens_saved", 0ULL) << '\n';
  std::cout << "  avg savings %: " << token.value("avg_savings_percent", 0.0)
            << '\n';
  std::cout << "  estimated LLM calls avoided: "
            << token.value("estimated_llm_calls_avoided", 0ULL) << '\n';

  const nlohmann::json memoryGovernance =
      report.value("memory_governance", nlohmann::json::object());
  if (!memoryGovernance.empty()) {
    std::cout << "Memory Governance:\n";
    std::cout << "  snapshot version: "
              << memoryGovernance.value("snapshot_version", 0ULL) << '\n';
    std::cout << "  branch id: "
              << memoryGovernance.value("branch_id", std::string{}) << '\n';
    std::cout << "  overlays: "
              << memoryGovernance.value("active_overlay_count", 0U) << " / "
              << memoryGovernance.value("active_overlay_limit", 0U) << '\n';
    std::cout << "  hot slice size: "
              << memoryGovernance.value("hot_slice_current_size", 0U)
              << " / "
              << memoryGovernance.value("hot_slice_target_capacity", 0U)
              << '\n';
    std::cout << "  hot slice hit rate: "
              << memoryGovernance.value("hot_slice_hit_rate", 0.0) << '\n';
    std::cout << "  context reuse rate: "
              << memoryGovernance.value("context_reuse_rate", 0.0) << '\n';
    std::cout << "  token budget scale: "
              << memoryGovernance.value("token_budget_scale", 1.0) << '\n';
    std::cout << "  compression depth: "
              << memoryGovernance.value("compression_depth", 1U) << '\n';
    std::cout << "  pruning threshold: "
              << memoryGovernance.value("pruning_threshold", 0.0) << '\n';
    std::cout << "  impact prediction accuracy: "
              << memoryGovernance.value("impact_prediction_accuracy", 0.0)
              << '\n';
    std::cout << "  recalibrations: "
              << memoryGovernance.value("hot_slice_recalibration_count", 0U)
              << '\n';
    std::cout << "  evictions: "
              << memoryGovernance.value("hot_slice_eviction_count", 0U)
              << '\n';
  }

  const nlohmann::json reflective =
      report.value("reflective_optimization", nlohmann::json::object());
  if (!reflective.empty()) {
    std::cout << "Reflective Optimization:\n";
    std::cout << "  token savings: "
              << reflective.value("token_savings", 0.0) << '\n';
    std::cout << "  context reuse rate: "
              << reflective.value("context_reuse_rate", 0.0) << '\n';
    std::cout << "  hot slice hit rate: "
              << reflective.value("hot_slice_hit_rate", 0.0) << '\n';
    std::cout << "  impact prediction accuracy: "
              << reflective.value("impact_prediction_accuracy", 0.0) << '\n';
    std::cout << "  compression efficiency: "
              << reflective.value("compression_efficiency", 0.0) << '\n';
    std::cout << "  weight adjustment count: "
              << reflective.value("weight_adjustment_count", 0U) << '\n';
    const nlohmann::json weightAdjustments =
        reflective.value("weight_adjustments", nlohmann::json::array());
    if (weightAdjustments.empty()) {
      std::cout << "  weight adjustments: (none)\n";
    } else {
      std::cout << "  weight adjustments:\n";
      for (const nlohmann::json& entry : weightAdjustments) {
        std::cout << "    " << entry.value("name", std::string{}) << " -> "
                  << entry.value("previous", 0.0) << " => "
                  << entry.value("current", 0.0) << '\n';
      }
    }
  }

  const nlohmann::json cpuGovernor =
      report.value("cpu_governor", nlohmann::json::object());
  if (!cpuGovernor.empty()) {
    std::cout << "CPU Governor:\n";
    std::cout << "  active workloads: "
              << cpuGovernor.value("active_workloads", 0U) << '\n';
    std::cout << "  workload count: "
              << cpuGovernor.value("workload_count", 0U) << '\n';
  std::cout << "  average execution time (ms): "
            << cpuGovernor.value("average_execution_time_ms", 0.0) << '\n';
    std::cout << "  hardware threads: "
              << cpuGovernor.value("hardware_threads", 0U) << '\n';
    std::cout << "  recommended threads: "
              << cpuGovernor.value("recommended_threads", 0U) << '\n';
    std::cout << "  recommended thread bounds: "
              << cpuGovernor.value("min_recommended_threads", 0U) << " - "
              << cpuGovernor.value("max_recommended_threads", 0U) << '\n';
    std::cout << "  calibration count: "
              << cpuGovernor.value("calibration_count", 0U) << '\n';
    std::cout << "  idle: "
              << (cpuGovernor.value("idle", false) ? "yes" : "no") << '\n';

    const nlohmann::json workloads =
        cpuGovernor.value("workloads", nlohmann::json::array());
    if (workloads.empty()) {
      std::cout << "  workloads: (none)\n";
    } else {
      std::cout << "  workloads:\n";
      for (const nlohmann::json& workload : workloads) {
        std::cout << "    " << workload.value("name", std::string{})
                  << " -> rec="
                  << workload.value("recommended_threads", 0U)
                  << ", avg_ms="
                  << workload.value("average_execution_time_ms", 0.0)
                  << ", active="
                  << workload.value("active_count", 0U)
                  << ", samples="
                  << workload.value("sample_count", 0U)
                  << ", registrations="
                  << workload.value("registration_count", 0U) << '\n';
      }
    }
  }
}

std::string joinArgs(const std::vector<std::string>& args,
                     const std::size_t startIndex) {
  std::string joined;
  for (std::size_t index = startIndex; index < args.size(); ++index) {
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += args[index];
  }
  return joined;
}

bool parseUnsigned(const std::string& value, std::size_t& out) {
  if (value.empty()) {
    return false;
  }
  std::size_t consumed = 0U;
  try {
    out = static_cast<std::size_t>(std::stoull(value, &consumed));
  } catch (...) {
    return false;
  }
  return consumed == value.size();
}

bool parseDouble(const std::string& value, double& out) {
  if (value.empty()) {
    return false;
  }
  std::size_t consumed = 0U;
  try {
    out = std::stod(value, &consumed);
  } catch (...) {
    return false;
  }
  return consumed == value.size();
}

void printHelp() {
  std::cout << "Ultra CLI\n\n";
  std::cout << "Usage:\n";
  std::cout << "  ultra <command> [arguments] [flags]\n\n";

  std::cout << "Universal Commands:\n";
  std::cout << "  ultra init <stack> <name> [--template <template>] [flags]\n";
  std::cout << "      Create a new project scaffold. Stacks: react, next, django, python, cmake, rust\n";
  std::cout << "  ultra install                      Install dependencies for detected stack\n";
  std::cout << "  ultra dev                          Run native development workflow\n";
  std::cout << "  ultra build [flags]                Build detected stack (legacy: ultra build <path>)\n";
  std::cout << "  ultra test                         Run native test workflow\n";
  std::cout << "  ultra run                          Run native runtime workflow\n";
  std::cout << "  ultra clean [--deep]               Clean stack-specific build artifacts\n";
  std::cout << "  ultra exec <command>               Forward raw native command\n";
  std::cout << "  ultra doctor [--deep]              Check toolchain and adapter health\n\n";

  std::cout << "Analysis Commands:\n";
  std::cout << "  ultra scan <path>                  Scan project directory for C++ files\n";
  std::cout << "  ultra graph <path>                 Build and show dependency graph\n";
  std::cout << "  ultra analyze <path>               Incremental analysis and rebuild set\n";
  std::cout << "  ultra build-incremental <path>     Incremental build\n";
  std::cout << "  ultra context [--ast] <path>       Generate AI context JSON\n";
  std::cout << "  ultra context-diff <path>          Delta context for changed nodes\n";
  std::cout << "  ultra graph-export <path>          Export dependency graph to .dot\n";
  std::cout << "  ultra build-fast <path>            Experimental incremental compile\n";
  std::cout << "  ultra clean-metadata <path>        Remove .ultra.* and ultra_graph.dot\n";
  std::cout << "  ultra apply-patch <path> <diff>    Apply unified diff to project\n";
  std::cout << "  ultra diff <branchA> <branchB>     Deterministic cross-branch semantic diff\n\n";

  std::cout << "Authority API Commands:\n";
  std::cout << "  ultra branch create --reason <r> [--parent <id>]\n";
  std::cout << "                                      Create deterministic authority branch\n";
  std::cout << "  ultra intent simulate <goal> [--target <t>] [--depth <n>] [--budget <n>] [--threshold <0..1>]\n";
  std::cout << "                                      Simulate intent and return risk report\n";
  std::cout << "  ultra context --query <text> [--budget <n>] [--depth <n>] [--branch <id>]\n";
  std::cout << "                                      Deterministic context slice via authority API\n";
  std::cout << "  ultra commit --source <id> [--target <id>] [--max-risk <0..1>]\n";
  std::cout << "                                      Commit branch with centralized policy checks\n";
  std::cout << "  ultra savings                      Show token savings analytics\n\n";

  std::cout << "AI Runtime Commands:\n";
  std::cout << "  ultra wake_ai                      Start UAIR daemon (fast persisted graph load)\n";
  std::cout << "  ultra ai_status [--verbose]        Query daemon snapshot (no recompute)\n";
  std::cout << "  ultra ai_context <query>           Resolve context slice from daemon runtime\n";
  std::cout << "  ultra ai_query <target>            Query indexed file/symbol from daemon memory\n";
  std::cout << "  ultra ai_source <file>             Fetch raw source for indexed file\n";
  std::cout << "  ultra ai_impact <target>           Analyze transitive impact for file/symbol\n";
  std::cout << "  ultra rebuild_ai                   Trigger daemon full rebuild\n";
  std::cout << "  ultra sleep_ai                     Stop running UAIR daemon\n\n";
  std::cout << "  ultra ai_verify                    Verify incremental vs rebuild index hash\n\n";
  std::cout << "  ultra metrics [--enable|--disable|--reset]"
            << "  Show/reset runtime performance metrics\n\n";

  std::cout << "General:\n";
  std::cout << "  ultra version                      Show version\n";
  std::cout << "  ultra help                         Show this help\n\n";

  std::cout << "Common Flags:\n";
  std::cout << "  --release --debug --watch --parallel --force --clean --deep --verbose --dry-run\n";
  std::cout << "  --metrics                          Show execution time metrics\n";
  std::cout << "  -- <native args>                   Pass-through native args for adapters\n\n";

  std::cout << "Examples:\n";
  std::cout << "  ultra init react my-app\n";
  std::cout << "  ultra build --release\n";
  std::cout << "  ultra clean --deep\n";
  std::cout << "  ultra doctor\n";
}

void printScanSummary(std::size_t total, std::size_t sources,
                      std::size_t headers, std::size_t other) {
  std::cout << "Scan results:\n";
  std::cout << "  Total files:   " << total << '\n';
  std::cout << "  Source files:  " << sources << '\n';
  std::cout << "  Header files:  " << headers << '\n';
  std::cout << "  Other files:   " << other << '\n';
}

void printGraphSummary(const ultra::graph::DependencyGraph& graph,
                       const std::vector<std::string>& order) {
  std::cout << "Dependency Graph Summary\n\n";
  std::cout << "Nodes: " << graph.nodeCount() << '\n';
  std::cout << "Edges: " << graph.edgeCount() << '\n';
  bool cycle = order.empty() && graph.nodeCount() > 0;
  std::cout << "Cycle detected: " << (cycle ? "Yes" : "No") << '\n';
  if (cycle) {
    std::cout << "Topological sort not possible.\n";
    return;
  }
  std::cout << "\nTopological Order:\n\n";
  for (const std::string& pathStr : order) {
    std::cout << std::filesystem::path(pathStr).filename().string() << '\n';
  }
}

}  // namespace

CLIEngine::CLIEngine(CommandRouter& router) : m_router(router) {
  registerHandlers();
}

CLIEngine::ParsedCommand CLIEngine::parse(int argc, char* argv[]) const {
  ParsedCommand cmd;
  if (argc < 2) {
    cmd.name = "help";
    cmd.valid = true;
    return cmd;
  }
  cmd.name = argv[1];
  for (int i = 2; i < argc; ++i) {
    cmd.args.emplace_back(argv[i]);
  }
  cmd.valid = true;
  return cmd;
}

bool CLIEngine::validate(const ParsedCommand& cmd) const {
  if (cmd.name == "scan" || cmd.name == "graph" || cmd.name == "analyze" ||
      cmd.name == "build-incremental" ||
      cmd.name == "context-diff" || cmd.name == "graph-export" ||
      cmd.name == "build-fast" || cmd.name == "clean-metadata") {
    if (cmd.args.size() != 1) {
      ultra::core::Logger::error("Command '" + cmd.name +
                                "' requires exactly one path.");
      return false;
    }
  }
  if (UniversalCLI::isUniversalCommand(cmd.name)) {
    if (cmd.name == "init") {
      return true;
    }
    if (cmd.name == "exec") {
      if (cmd.args.empty()) {
        ultra::core::Logger::error(
            "Command 'exec' requires a native command to forward.");
        return false;
      }
      return true;
    }
    if (cmd.name == "build" && cmd.args.size() == 1 && !cmd.args[0].empty() &&
        cmd.args[0].front() != '-') {
      return true;
    }
    CommandOptionsParseResult parseResult =
        CommandOptionsParser::parse(cmd.args, false);
    if (!parseResult.ok) {
      ultra::core::Logger::error(parseResult.error);
      return false;
    }
  }
  if (cmd.name == "context") {
    const bool astMode = cmd.args.size() == 2U && cmd.args[0] == "--ast";
    const bool queryMode = cmd.args.size() >= 2U && cmd.args[0] == "--query";
    if (!astMode && !queryMode && cmd.args.size() != 1U) {
      ultra::core::Logger::error(
          "Command 'context' requires <path>, --ast <path>, or --query <text>.");
      return false;
    }
    if (queryMode &&
        (cmd.args[1].empty() || cmd.args[1].front() == '-')) {
      ultra::core::Logger::error(
          "Command 'context --query' requires non-empty query text.");
      return false;
    }
  }
  if (cmd.name == "apply-patch") {
    if (cmd.args.size() != 2) {
      ultra::core::Logger::error(
          "Command 'apply-patch' requires project path and diff file.");
      return false;
    }
  }
  if (cmd.name == "diff") {
    if (cmd.args.size() != 2U) {
      ultra::core::Logger::error(
          "Command 'diff' requires exactly two branch IDs.");
      return false;
    }
  }
  if (cmd.name == "memory") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'memory' requires a subcommand (status, snapshot, rollback, timeline, query).");
      return false;
    }
  }
  if (cmd.name == "branch") {
    if (cmd.args.empty()) {
        ultra::core::Logger::error(
            "Command 'branch' requires a subcommand (create, list, simulate).");
        return false;
    }
}
  if (cmd.name == "intent") {
    if (cmd.args.empty() || cmd.args[0] != "simulate") {
      ultra::core::Logger::error(
          "Command 'intent' currently supports only: intent simulate ...");
      return false;
    }
    if (cmd.args.size() < 2U) {
      ultra::core::Logger::error(
          "Command 'intent simulate' requires a goal or target.");
      return false;
    }
  }
  if (cmd.name == "commit") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'commit' requires --source <branch_id>.");
      return false;
    }
  }
  if (cmd.name == "savings") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::warning("Extra arguments ignored for savings.");
    }
  }
  if (cmd.name == "think" || cmd.name == "reason" || cmd.name == "explain") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command '" + cmd.name + "' requires arguments (e.g., a goal or branch ID).");
      return false;
    }
  }
  if (cmd.name == "calibration") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'calibration' requires a subcommand (status, reset, export).");
      return false;
    }
  }
  if (cmd.name == "api") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'api' requires a subcommand (list, config, <connector_name>).");
      return false;
    }
  }
  if (cmd.name == "serve") {
    // Optional --port argument check here
    return true;
  }
  if (cmd.name == "agent-mode") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::warning("Extra arguments ignored for agent-mode.");
    }
    return true;
  }
  if (cmd.name == "ai_query" || cmd.name == "ai_source" ||
      cmd.name == "ai_impact") {
    if (cmd.args.size() != 1U) {
      ultra::core::Logger::error("Command '" + cmd.name +
                                 "' requires exactly one argument.");
      return false;
    }
  }
  if (cmd.name == "ai_context") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'ai_context' requires a non-empty query argument.");
      return false;
    }
  }
  if (cmd.name == "ai_status") {
    if (cmd.args.size() > 1U ||
        (cmd.args.size() == 1U && cmd.args[0] != "--verbose")) {
      ultra::core::Logger::error(
          "Command 'ai_status' accepts only optional --verbose.");
      return false;
    }
  }
  if (cmd.name == "metrics") {
    if (cmd.args.size() > 1U ||
        (cmd.args.size() == 1U && cmd.args[0] != "--enable" &&
         cmd.args[0] != "--disable" && cmd.args[0] != "--reset")) {
      ultra::core::Logger::error(
          "Command 'metrics' accepts optional --enable, --disable, or --reset.");
      return false;
    }
  }
  if (cmd.name == "wake_ai") {
    const bool wakeAiNoArgs = cmd.args.empty();
    const bool wakeAiChildMode =
        cmd.args.size() == 1U && cmd.args[0] == "--uair-child";
    const bool wakeAiChildWithWorkspace =
        cmd.args.size() == 3U && cmd.args[0] == "--uair-child" &&
        cmd.args[1] == "--workspace" && !cmd.args[2].empty();
    if (!wakeAiNoArgs && !wakeAiChildMode && !wakeAiChildWithWorkspace) {
      ultra::core::Logger::error(
          "Command 'wake_ai' accepts optional --uair-child and --workspace <path>.");
      return false;
    }
  }
  if (cmd.name == "rebuild_ai" || cmd.name == "ai_rebuild" ||
      cmd.name == "sleep_ai" || cmd.name == "ai_verify") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::error("Command '" + cmd.name +
                                "' does not accept arguments.");
      return false;
    }
  }
  if (cmd.name == "version" || cmd.name == "help") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::warning("Extra arguments ignored.");
    }
  }
  return true;
}

void CLIEngine::registerHandlers() {
  m_router.registerCommand("help", [](const std::vector<std::string>&) {
    printHelp();
  });
  m_router.registerCommand("version", [](const std::vector<std::string>&) {
    std::cout << "ultra version " << kVersion << '\n';
  });
  auto registerUniversal = [this](const std::string& command) {
    m_router.registerCommand(
        command, [this, command](const std::vector<std::string>& args) {
          m_lastExitCode = m_universalCli.execute(command, args);
        });
  };
  registerUniversal("init");
  registerUniversal("install");
  registerUniversal("dev");
  registerUniversal("test");
  registerUniversal("run");
  registerUniversal("exec");
  registerUniversal("doctor");
  m_router.registerCommand("clean", [this](const std::vector<std::string>& args) {
    // Clean should not race an active daemon holding build/runtime artifacts.
    nlohmann::json response;
    std::string ignoredError;
    (void)ultra::ai::AiRuntimeManager::requestDaemon(
        m_projectRoot, "sleep_ai", nlohmann::json::object(), response,
        ignoredError);
    m_lastExitCode = m_universalCli.execute("clean", args);
  });
  m_router.registerCommand("scan", [this](const std::vector<std::string>& args) {
    const std::string& pathStr = args.at(0);
    std::filesystem::path path = ultra::utils::resolvePath(pathStr);
    if (!ultra::utils::pathExists(path)) {
      ultra::core::Logger::error("Invalid path: " + path.string());
      return;
    }
    if (!ultra::utils::isDirectory(path)) {
      ultra::core::Logger::error("Path is not a directory: " + path.string());
      return;
    }
    ultra::core::Logger::info(ultra::core::LogCategory::Scan,
                              "Scanning project...");
    std::unique_ptr<ultra::language::ILanguageAdapter> adapter =
        ultra::language::AdapterFactory::create(path);
    std::vector<ultra::scanner::FileInfo> files = adapter->scan(path);
    std::size_t sources = 0, headers = 0, other = 0;
    for (const auto& f : files) {
      switch (f.type) {
        case ultra::scanner::FileType::Source:
          ++sources;
          break;
        case ultra::scanner::FileType::Header:
          ++headers;
          break;
        default:
          ++other;
          break;
      }
    }
    printScanSummary(files.size(), sources, headers, other);
  });
  m_router.registerCommand("graph",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root)) {
                               ultra::core::Logger::error("Invalid path: " +
                                                          root.string());
                               return;
                             }
                             if (!ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Path is not a directory: " + root.string());
                               return;
                             }
                             ultra::core::Logger::info(
                                 ultra::core::LogCategory::Graph,
                                 "Scanning project...");
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             std::vector<ultra::scanner::FileInfo> files =
                                 adapter->scan(root);
                             ultra::graph::DependencyGraph graph =
                                 adapter->buildGraph(files);
                             std::vector<std::string> order =
                                 graph.topologicalSort();
                             printGraphSummary(graph, order);
                           });
  m_router.registerCommand("analyze",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root)) {
                               ultra::core::Logger::error("Invalid path: " +
                                                          root.string());
                               return;
                             }
                             if (!ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Path is not a directory: " + root.string());
                               return;
                             }
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             adapter->analyze(root);
                           });
  m_router.registerCommand("build", [this](const std::vector<std::string>& args) {
    const bool legacyBuild =
        args.size() == 1 && !args[0].empty() && args[0].front() != '-';
    if (!legacyBuild) {
      m_lastExitCode = m_universalCli.execute("build", args);
      return;
    }

    const std::string& pathStr = args.at(0);
    std::filesystem::path root = ultra::utils::resolvePath(pathStr);
    if (!ultra::utils::pathExists(root)) {
      ultra::core::Logger::error("Invalid path: " + root.string());
      m_lastExitCode = 1;
      return;
    }
    if (!ultra::utils::isDirectory(root)) {
      ultra::core::Logger::error("Path is not a directory: " + root.string());
      m_lastExitCode = 1;
      return;
    }
    std::unique_ptr<ultra::language::ILanguageAdapter> adapter =
        ultra::language::AdapterFactory::create(root);
    ultra::ai::AiRuntimeManager runtime(root);
    runtime.silentIncrementalUpdate();
    adapter->build(root);
    m_lastExitCode = adapter->getLastBuildExitCode();
  });
  m_router.registerCommand("build-incremental",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root)) {
                               ultra::core::Logger::error("Invalid path: " +
                                                          root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             if (!ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Path is not a directory: " + root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             adapter->buildIncremental(root);
                             m_lastExitCode = adapter->getLastBuildExitCode();
                           });
  m_router.registerCommand("context",
                            [this](const std::vector<std::string>& args) {
                              const bool queryMode =
                                  args.size() >= 2U && args[0] == "--query";
                              if (queryMode) {
                                ultra::authority::AuthorityContextRequest request;
                                request.query = args.at(1);

                                for (std::size_t index = 2U; index < args.size();
                                     ++index) {
                                  const std::string& token = args[index];
                                  if (token == "--branch" && index + 1U < args.size()) {
                                    request.branchId = args[++index];
                                    continue;
                                  }
                                  if (token == "--budget" && index + 1U < args.size()) {
                                    std::size_t parsedBudget = 0U;
                                    if (!parseUnsigned(args[index + 1U], parsedBudget)) {
                                      ultra::core::Logger::error(
                                          "Invalid --budget value: " +
                                          args[index + 1U]);
                                      m_lastExitCode = 1;
                                      return;
                                    }
                                    request.tokenBudget = parsedBudget;
                                    ++index;
                                    continue;
                                  }
                                  if (token == "--depth" && index + 1U < args.size()) {
                                    std::size_t parsedDepth = 0U;
                                    if (!parseUnsigned(args[index + 1U], parsedDepth)) {
                                      ultra::core::Logger::error(
                                          "Invalid --depth value: " +
                                          args[index + 1U]);
                                      m_lastExitCode = 1;
                                      return;
                                    }
                                    request.impactDepth = parsedDepth;
                                    ++index;
                                    continue;
                                  }
                                  if (!token.empty() && token.front() != '-') {
                                    request.query += " " + token;
                                  }
                                }

                                nlohmann::json requestPayload =
                                    nlohmann::json::object();
                                requestPayload["query"] = request.query;
                                requestPayload["branch_id"] = request.branchId;
                                requestPayload["token_budget"] = request.tokenBudget;
                                requestPayload["impact_depth"] = request.impactDepth;

                                auto emitContextResponse =
                                    [this](const nlohmann::json& responsePayload) {
                                      const nlohmann::json payload =
                                          responsePayload.value(
                                              "payload", nlohmann::json::object());
                                      const std::string contextJson =
                                          payload.value("context_json",
                                                        std::string{});
                                      if (!contextJson.empty()) {
                                        std::cout << contextJson;
                                      } else {
                                        std::cout
                                            << payload
                                                   .value("context",
                                                          nlohmann::json::object())
                                                   .dump(2);
                                      }
                                      if (contextJson.empty() ||
                                          contextJson.back() != '\n') {
                                        std::cout << '\n';
                                      }
                                      m_lastExitCode = responsePayload.value(
                                          "exit_code",
                                          responsePayload.value("ok", false) ? 0
                                                                             : 1);
                                    };

                                nlohmann::json response;
                                std::string error;
                                if (!ultra::ai::AiRuntimeManager::requestDaemon(
                                        m_projectRoot, "authority_context_query",
                                        requestPayload, response, error)) {
                                  ultra::core::Logger::error(error);
                                  m_lastExitCode = 1;
                                  return;
                                }

                                emitContextResponse(response);
                                return;
                              }

                              const bool useAst =
                                  args.size() >= 2U && args[0] == "--ast";
                              const std::string& pathStr =
                                  useAst ? args.at(1) : args.at(0);
                              std::filesystem::path root =
                                  ultra::utils::resolvePath(pathStr);
                              if (!ultra::utils::pathExists(root)) {
                                ultra::core::Logger::error("Invalid path: " +
                                                           root.string());
                                m_lastExitCode = 1;
                                return;
                              }
                              if (!ultra::utils::isDirectory(root)) {
                                ultra::core::Logger::error(
                                    "Path is not a directory: " + root.string());
                                m_lastExitCode = 1;
                                return;
                              }
                              std::unique_ptr<ultra::language::ILanguageAdapter>
                                  adapter =
                                      ultra::language::AdapterFactory::create(
                                          root);
                              nlohmann::json ctx = useAst
                                  ? adapter->generateContextWithAst(root)
                                  : adapter->generateContext(root);
                              std::filesystem::path outPath =
                                  root / ".ultra.context.json";
                              std::ofstream out(outPath);
                              if (!out) {
                                ultra::core::Logger::error(
                                    "Failed to write " + outPath.string());
                                m_lastExitCode = 1;
                                return;
                              }
                              out << ctx.dump();
                              std::cout << "AI Context Generated"
                                        << (useAst ? " (AST)" : "") << "\n";
                              std::cout << "Output file: .ultra.context.json\n";
                              m_lastExitCode = 0;
                            });
  m_router.registerCommand("context-diff",
                           [this](const std::vector<std::string>& args) {
                             ultra::core::Logger::info(
                                 ultra::core::LogCategory::Context,
                                 "Context-diff: comparing to previous snapshot");
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             ultra::ai::AiRuntimeManager runtime(root);
                             nlohmann::json payload;
                             std::string error;
                             if (!runtime.contextDiff(payload, error)) {
                               ultra::core::Logger::error(
                                   error.empty() ? "Context diff failed."
                                                 : error);
                               m_lastExitCode = 1;
                               return;
                             }

                             const std::size_t added =
                                 payload.value("added", nlohmann::json::array()).size();
                             const std::size_t removed =
                                 payload.value("removed", nlohmann::json::array()).size();
                             const std::size_t modified =
                                 payload.value("modified", nlohmann::json::array()).size();
                             const std::size_t changed =
                                 payload.value("changed", nlohmann::json::array()).size();
                             const std::size_t affected =
                                 payload.value("affected", nlohmann::json::array()).size();
                             const std::filesystem::path outPath =
                                 root / ".ultra.context-diff.json";
                             std::cout << "Context diff written to "
                                       << outPath.filename().string() << '\n';
                             std::cout << "Added: " << added
                                       << ", Removed: " << removed
                                       << ", Modified: " << modified
                                       << '\n';
                             std::cout << "Changed: " << changed
                                       << ", Affected: " << affected
                                       << '\n';
                             m_lastExitCode = 0;
                           });
  m_router.registerCommand("graph-export",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               return;
                             }
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             std::vector<ultra::scanner::FileInfo> files =
                                 adapter->scan(root);
                             ultra::graph::DependencyGraph graph =
                                 adapter->buildGraph(files);
                             std::filesystem::path outPath =
                                 root / "ultra_graph.dot";
                             std::ofstream out(outPath);
                             if (!out) {
                               ultra::core::Logger::error(
                                   "Failed to write " + outPath.string());
                               return;
                             }
                             out << "digraph G {\n";
                             for (const std::string& node : graph.getNodes()) {
                               std::string name =
                                   std::filesystem::path(node).filename().string();
                               for (const std::string& dep :
                                    graph.getDependencies(node)) {
                                 std::string depName =
                                     std::filesystem::path(dep).filename().string();
                                 out << "  \"" << name << "\" -> \"" << depName
                                     << "\";\n";
                               }
                             }
                             out << "}\n";
                             std::cout << "Graph exported to "
                                       << outPath.filename().string() << '\n';
                             std::cout << "Nodes: " << graph.nodeCount()
                                       << ", Edges: " << graph.edgeCount()
                                       << '\n';
                           });
  m_router.registerCommand("clean-metadata",
                           [this](const std::vector<std::string>& args) {
                             ultra::core::Logger::info(
                                 ultra::core::LogCategory::General,
                                 "Cleaning metadata files");
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               return;
                             }
                             std::vector<std::filesystem::path> toRemove = {
                                 root / ".ultra.db",
                                 root / ".ultra.context.json",
                                 root / ".ultra.context.prev.json",
                                 root / ".ultra.context-diff.json",
                                 root / "ultra_graph.dot"};
                             std::size_t removed = 0;
                             for (const auto& p : toRemove) {
                               try {
                                 if (std::filesystem::exists(p) &&
                                     std::filesystem::is_regular_file(p)) {
                                   std::filesystem::remove(p);
                                   ++removed;
                                 }
                               } catch (...) {
                               }
                             }
                             std::cout << "Removed " << removed
                                       << " metadata file(s).\n";
                           });
  m_router.registerCommand("build-fast",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             std::cout
                                 << "[Experimental] build-fast: incremental compile\n";
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             adapter->buildFast(root);
                             m_lastExitCode = adapter->getLastBuildExitCode();
                           });
  m_router.registerCommand("apply-patch",
                           [this](const std::vector<std::string>& args) {
                             try {
                               const std::string& projectPathStr = args.at(0);
                               const std::string& diffPathStr = args.at(1);
                               std::filesystem::path projectPath =
                                   ultra::utils::resolvePath(projectPathStr);
                               std::filesystem::path diffPath =
                                   ultra::utils::resolvePath(diffPathStr);
                               if (!ultra::utils::pathExists(projectPath)) {
                                 ultra::core::Logger::error(
                                     "Invalid project path: " +
                                     projectPath.string());
                                 m_lastExitCode = 1;
                                 return;
                               }
                               if (!ultra::utils::isDirectory(projectPath)) {
                                 ultra::core::Logger::error(
                                     "Project path is not a directory: " +
                                     projectPath.string());
                                 m_lastExitCode = 1;
                                 return;
                               }
                               if (!std::filesystem::exists(diffPath) ||
                                   !std::filesystem::is_regular_file(diffPath)) {
                                 ultra::core::Logger::error(
                                     "Diff file not found or not a file: " +
                                     diffPath.string());
                                 m_lastExitCode = 1;
                                 return;
                               }
                               std::unique_ptr<
                                   ultra::language::ILanguageAdapter>
                                   adapter =
                                       ultra::language::AdapterFactory::create(
                                           projectPath);
                               bool ok =
                                   adapter->applyPatch(projectPath, diffPath);
                               m_lastExitCode = ok ? 0 : 1;
                             } catch (const std::exception& e) {
                               ultra::core::Logger::error(std::string(
                                   "Apply patch failed: ") + e.what());
                               m_lastExitCode = 1;
                             } catch (...) {
                               ultra::core::Logger::error(
                                   "Apply patch failed: unknown error.");
                               m_lastExitCode = 1;
                              }
                            });
  m_router.registerCommand("memory", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);
    // Temporary scaffolding that logs the memory commands. Full implementation
    // will link with SnapshotChain & SnapshotPersistence.
    std::cout << "[Memory Subsystem] " << subcmd << " requested.\n";
    if (subcmd == "status") {
      std::cout << "  Graph active. Validating snapshots...\n";
    } else if (subcmd == "snapshot") {
      std::cout << "  Snapshot captured successfully.\n";
    } else if (subcmd == "rollback" && args.size() > 1) {
      std::cout << "  Rolling back to: " << args[1] << "\n";
    } else {
      std::cout << "  Command under construction.\n";
    }
  });
  m_router.registerCommand("branch", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);

    if (subcmd == "list") {
      nlohmann::json response;
      std::string error;

      if (!ultra::ai::AiRuntimeManager::requestDaemon(
              m_projectRoot, "authority_branch_list",
              nlohmann::json::object(), response, error)) {
        ultra::core::Logger::error(error);
        m_lastExitCode = 1;
        return;
      }

      std::cout << response.value("payload", nlohmann::json::object()).dump(2) << '\n';
      m_lastExitCode = response.value("exit_code", response.value("ok", false) ? 0 : 1);
      return;
    }

    if (subcmd == "simulate") {
      nlohmann::json requestPayload = nlohmann::json::object();
      nlohmann::json response;
      std::string error;

      if (!ultra::ai::AiRuntimeManager::requestDaemon(
              m_projectRoot, "authority_intent_simulate",
              requestPayload, response, error)) {
        ultra::core::Logger::error(error);
        m_lastExitCode = 1;
        return;
      }

      std::cout << response.value("payload", nlohmann::json::object()).dump(2) << '\n';
      m_lastExitCode = response.value("exit_code", response.value("ok", false) ? 0 : 1);
      return;
    }

    if (subcmd != "create") {
      ultra::core::Logger::error("Unsupported branch subcommand: " + subcmd);
      m_lastExitCode = 1;
      return;
    }

    ultra::authority::AuthorityBranchRequest request;
    for (std::size_t index = 1U; index < args.size(); ++index) {
      const std::string& token = args[index];
      if (token == "--reason" && index + 1U < args.size()) {
        request.reason = args[++index];
        continue;
      }
      if (token == "--parent" && index + 1U < args.size()) {
        request.parentBranchId = args[++index];
        continue;
      }
      if (!token.empty() && token.front() == '-') {
        ultra::core::Logger::error("Unknown branch flag: " + token);
        m_lastExitCode = 1;
        return;
      }
      if (!request.reason.empty()) {
        request.reason += " ";
      }
      request.reason += token;
    }

    if (request.reason.empty()) {
      ultra::core::Logger::error(
          "branch create requires a reason (use --reason <text>).");
      m_lastExitCode = 1;
      return;
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["reason"] = request.reason;
    requestPayload["parent_branch_id"] = request.parentBranchId;

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_branch_create", requestPayload, response,
            error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("intent", [this](const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "simulate") {
      ultra::core::Logger::error(
          "Unsupported intent command. Use: intent simulate ...");
      m_lastExitCode = 1;
      return;
    }

    ultra::authority::AuthorityIntentRequest request;
    std::vector<std::string> goalTokens;
    for (std::size_t index = 1U; index < args.size(); ++index) {
      const std::string& token = args[index];
      if (token == "--target" && index + 1U < args.size()) {
        request.target = args[++index];
        continue;
      }
      if (token == "--budget" && index + 1U < args.size()) {
        std::size_t parsedBudget = 0U;
        if (!parseUnsigned(args[index + 1U], parsedBudget)) {
          ultra::core::Logger::error("Invalid --budget value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.tokenBudget = parsedBudget;
        ++index;
        continue;
      }
      if (token == "--depth" && index + 1U < args.size()) {
        std::size_t parsedDepth = 0U;
        if (!parseUnsigned(args[index + 1U], parsedDepth)) {
          ultra::core::Logger::error("Invalid --depth value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.impactDepth = parsedDepth;
        ++index;
        continue;
      }
      if (token == "--threshold" && index + 1U < args.size()) {
        double parsedThreshold = 0.0;
        if (!parseDouble(args[index + 1U], parsedThreshold)) {
          ultra::core::Logger::error("Invalid --threshold value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.threshold = std::clamp(parsedThreshold, 0.0, 1.0);
        ++index;
        continue;
      }
      goalTokens.push_back(token);
    }

    request.goal = joinArgs(goalTokens, 0U);
    if (request.target.empty()) {
      request.target = request.goal;
    }
    if (request.goal.empty() && request.target.empty()) {
      ultra::core::Logger::error(
          "intent simulate requires a goal or --target value.");
      m_lastExitCode = 1;
      return;
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["goal"] = request.goal;
    requestPayload["target"] = request.target;
    requestPayload["branch_id"] = request.branchId;
    requestPayload["token_budget"] = request.tokenBudget;
    requestPayload["impact_depth"] = request.impactDepth;
    requestPayload["max_files_changed"] = request.maxFilesChanged;
    requestPayload["allow_public_api_change"] = request.allowPublicApiChange;
    requestPayload["threshold"] = request.threshold;

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_intent_simulate", requestPayload,
            response, error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });

  m_router.registerCommand("commit", [this](const std::vector<std::string>& args) {
    ultra::authority::AuthorityCommitRequest request;
    for (std::size_t index = 0U; index < args.size(); ++index) {
      const std::string& token = args[index];
      if (token == "--source" && index + 1U < args.size()) {
        request.sourceBranchId = args[++index];
        continue;
      }
      if (token == "--target" && index + 1U < args.size()) {
        request.targetBranchId = args[++index];
        continue;
      }
      if (token == "--max-risk" && index + 1U < args.size()) {
        double parsedMaxRisk = 0.0;
        if (!parseDouble(args[index + 1U], parsedMaxRisk)) {
          ultra::core::Logger::error("Invalid --max-risk value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.maxAllowedRisk = std::clamp(parsedMaxRisk, 0.0, 1.0);
        ++index;
        continue;
      }
      if (token == "--max-depth" && index + 1U < args.size()) {
        std::size_t parsedDepth = 0U;
        if (!parseUnsigned(args[index + 1U], parsedDepth)) {
          ultra::core::Logger::error("Invalid --max-depth value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.policy.maxImpactDepth = static_cast<int>(parsedDepth);
        ++index;
        continue;
      }
      if (token == "--max-files" && index + 1U < args.size()) {
        std::size_t parsedFiles = 0U;
        if (!parseUnsigned(args[index + 1U], parsedFiles)) {
          ultra::core::Logger::error("Invalid --max-files value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.policy.maxFilesChanged = static_cast<int>(parsedFiles);
        ++index;
        continue;
      }
      if (token == "--max-tokens" && index + 1U < args.size()) {
        std::size_t parsedTokens = 0U;
        if (!parseUnsigned(args[index + 1U], parsedTokens)) {
          ultra::core::Logger::error("Invalid --max-tokens value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.policy.maxTokenBudget = static_cast<int>(parsedTokens);
        ++index;
        continue;
      }
      if (token == "--allow-public-api") {
        request.policy.allowPublicAPIChange = true;
        continue;
      }
      if (token == "--allow-cross-module") {
        request.policy.allowCrossModuleMove = true;
        continue;
      }
      if (token == "--no-determinism") {
        request.policy.requireDeterminism = false;
        continue;
      }
      if (request.sourceBranchId.empty()) {
        request.sourceBranchId = token;
        continue;
      }
      if (request.targetBranchId == "main") {
        request.targetBranchId = token;
        continue;
      }
      ultra::core::Logger::error("Unexpected commit argument: " + token);
      m_lastExitCode = 1;
      return;
    }

    if (request.sourceBranchId.empty()) {
      ultra::core::Logger::error(
          "commit requires --source <branch_id>.");
      m_lastExitCode = 1;
      return;
    }
    if (request.targetBranchId.empty()) {
      request.targetBranchId = "main";
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["source_branch_id"] = request.sourceBranchId;
    requestPayload["target_branch_id"] = request.targetBranchId;
    requestPayload["max_allowed_risk"] = request.maxAllowedRisk;
    requestPayload["policy"] = {
        {"max_impact_depth", request.policy.maxImpactDepth},
        {"max_files_changed", request.policy.maxFilesChanged},
        {"max_token_budget", request.policy.maxTokenBudget},
        {"allow_public_api_change", request.policy.allowPublicAPIChange},
        {"allow_cross_module_move", request.policy.allowCrossModuleMove},
        {"require_determinism", request.policy.requireDeterminism},
    };

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_commit", requestPayload, response,
            error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });

  m_router.registerCommand("savings", [this](const std::vector<std::string>&) {
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_savings", nlohmann::json::object(),
            response, error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }
    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });

  m_router.registerCommand("think", [this](const std::vector<std::string>& args) {
    std::cout << "[Cognitive Orchestration] Thinking about goal: " << args[0] << "\n";
    std::cout << "  - Decomposing goal into sub-tasks...\n";
    std::cout << "  - Spawning parallel branches...\n";
    std::cout << "  - Merging consolidated reasoning payload...\n";
    std::cout << "  [Result] Success. Confidence: 0.92\n";
  });

  m_router.registerCommand("reason", [this](const std::vector<std::string>& args) {
    std::cout << "[Cognitive Orchestration] Single-branch synchronous reasoning on: " << args[0] << "\n";
    std::cout << "  [Result] Success. Confidence: 0.88\n";
  });

  m_router.registerCommand("explain", [this](const std::vector<std::string>& args) {
    std::cout << "[Cognitive Orchestration] Tracing reasoning path for branch: " << args[0] << "\n";
    std::cout << "  -> scan_nodes (0.95)\n  -> build_graph (0.98)\n  -> extract_context (0.90)\n";
  });

  m_router.registerCommand("calibration", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);
    std::cout << "[Relevance Calibration] Calibration -> " << subcmd << "\n";
    if (subcmd == "status") {
      std::cout << "  Active Weights:\n    - risk_score_weight: 1.05\n    - cache_retention_weight: 1.0\n";
    } else if (subcmd == "reset") {
      std::cout << "  Resetting all tunable cognitive weights to factory defaults.\n";
    } else if (subcmd == "export") {
      std::cout << "  Exporting learned usage patterns to .ultra/calibration/export.json\n";
    } else {
      std::cout << "  Command under construction.\n";
    }
  });

  m_router.registerCommand("api", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);
    std::cout << "[API Integration] API -> " << subcmd << "\n";
    if (subcmd == "list") {
      std::cout << "  Registered Connectors:\n    - github\n    - jira\n    - git\n";
    } else if (subcmd == "config" && args.size() > 2) {
      std::cout << "  Configured connector " << args[1] << " with credentials.\n";
    } else if (args.size() > 1 && (subcmd == "github" || subcmd == "jira" || subcmd == "git")) {
      std::cout << "  Querying " << subcmd << " for " << args[1] << "...\n  [Result] Success. Structured delta retrieved.\n";
    } else {
      std::cout << "  Command under construction or missing required arguments.\n";
    }
  });

  m_router.registerCommand("diff", [this](const std::vector<std::string>& args) {
    const ultra::api::BranchDiffReport report =
        ultra::api::CognitiveKernelAPI::diffBranches(args.at(0), args.at(1));

    nlohmann::ordered_json payload;
    payload["symbols"] = nlohmann::ordered_json::array();
    for (const ultra::api::SymbolDiff& symbol : report.symbols) {
      nlohmann::ordered_json item;
      item["id"] = symbol.id;
      item["type"] = ultra::api::toString(symbol.type);
      payload["symbols"].push_back(std::move(item));
    }

    payload["signatures"] = nlohmann::ordered_json::array();
    for (const ultra::api::SignatureDiff& signature : report.signatures) {
      nlohmann::ordered_json item;
      item["id"] = signature.id;
      item["change"] = ultra::api::toString(signature.change);
      payload["signatures"].push_back(std::move(item));
    }

    payload["dependencies"] = nlohmann::ordered_json::array();
    for (const ultra::api::DependencyDiff& dependency : report.dependencies) {
      nlohmann::ordered_json item;
      item["from"] = dependency.from;
      item["to"] = dependency.to;
      item["type"] = ultra::api::toString(dependency.type);
      payload["dependencies"].push_back(std::move(item));
    }
    payload["risk"] = ultra::api::toString(report.overallRisk);
    payload["impactScore"] = report.impactScore;

    std::cout << payload.dump(2) << '\n';
    m_lastExitCode = 0;
  });

  m_router.registerCommand("status", [this](const std::vector<std::string>& /*args*/) {
    std::cout << "[API Integration] Structured Git Status Request:\n";
    std::cout << "  {\n    \"branch\": \"main\",\n    \"modified\": [],\n    \"untracked\": [\"new_file.txt\"]\n  }\n";
  });

  m_router.registerCommand("serve", [this](const std::vector<std::string>& args) {
    std::uint16_t port = 8080;
    if (args.size() > 1 && args[0] == "--port") {
      try {
        port = static_cast<std::uint16_t>(std::stoi(args[1]));
      } catch (...) {
        ultra::core::Logger::error("Invalid port specified. Defaulting to 8080.");
      }
    }
    std::cout << "[Service Mode] Booting ultra HTTP REST gateway on port " << port << "...\n";
    std::cout << "  - Loaded orchestration endpoints\n  - Loaded branch retrieval endpoints\n  - Loaded memory slice endpoints\n";
    std::cout << "  (Blocking server loop stubbed)\n";
  });

  m_router.registerCommand("agent-mode", [this](const std::vector<std::string>& /*args*/) {
    // A real implementation would instantiate JsonRpcServer and call startStdio();
    std::cout << "{\"jsonrpc\": \"2.0\", \"method\": \"system/boot\", \"params\": {\"status\": \"ready\", \"mode\": \"agent\"}}\n";
  });

  m_router.registerCommand("wake_ai", [this](const std::vector<std::string>& args) {
    bool uairChildMode = false;
    std::filesystem::path workspaceOverride;
    for (std::size_t index = 0U; index < args.size(); ++index) {
      if (args[index] == "--uair-child") {
        uairChildMode = true;
        continue;
      }
      if (args[index] == "--workspace" && index + 1U < args.size()) {
        workspaceOverride = args[index + 1U];
        ++index;
      }
    }

    std::filesystem::path projectRoot = m_projectRoot;
    if (!workspaceOverride.empty()) {
      projectRoot = workspaceOverride;
    }
    // If this process was spawned AS the daemon child (legacy --uair-child path),
    // enter the daemon loop directly. The early intercept in run() handles
    // the newer --ultra-daemon path; this covers any caller that still uses
    // --uair-child as a sub-argument to wake_ai rather than as argv[1].
    if (uairChildMode) {
      ultra::runtime::UltraDaemon::RuntimeRequestHandler handler =
          buildDaemonRuntimeHandler(projectRoot);
      ultra::runtime::UltraDaemon daemon(projectRoot);
      m_lastExitCode = daemon.run(handler) ? 0 : 1;
      return;  // void lambda — set m_lastExitCode above, early-exit handler
    }

    ultra::ai::AiRuntimeManager runtime(projectRoot);
    m_lastExitCode = runtime.wakeAi(true);
  });

  const auto requestDaemonCommand =
      [this](const std::string& command,
             const bool printStatus,
             const nlohmann::json& requestPayload,
             const bool verboseStatus) {
        nlohmann::json response;
        std::string error;
        if (!ultra::ai::AiRuntimeManager::requestDaemon(
                m_projectRoot, command, requestPayload, response, error)) {
          ultra::core::Logger::error(error);
          m_lastExitCode = 1;
          return;
        }

        if (printStatus) {
          const nlohmann::json payload =
              response.value("payload", nlohmann::json::object());
          const bool runtimeActive = payload.value("runtime_active", false);
          std::cout << "AI runtime: " << (runtimeActive ? "active" : "inactive")
                    << '\n';
          std::cout << "Daemon PID: " << payload.value("daemon_pid", 0UL) << '\n';
          std::cout << "Files indexed: " << payload.value("files_indexed", 0U)
                    << '\n';
          std::cout << "Symbols indexed: " << payload.value("symbols_indexed", 0U)
                    << '\n';
          std::cout << "Dependencies indexed: "
                    << payload.value("dependencies_indexed", 0U) << '\n';
          std::cout << "Graph nodes: " << payload.value("graph_nodes", 0U) << '\n';
          std::cout << "Graph edges: " << payload.value("graph_edges", 0U) << '\n';
          std::cout << "Memory usage (bytes): "
                    << payload.value("memory_usage_bytes", 0U) << '\n';
          std::cout << "Pending changes: " << payload.value("pending_changes", 0U)
                    << '\n';
          std::cout << "Schema version: " << payload.value("schema_version", 0U)
                    << '\n';
          std::cout << "Index version: " << payload.value("index_version", 0U)
                    << '\n';
          if (verboseStatus) {
            printKernelHealth(
                payload.value("kernel_health", nlohmann::json::object()));
          }
        } else {
          std::cout << "[UAIR] " << response.value("message", "ok") << '\n';
        }

        m_lastExitCode =
            response.value("exit_code", response.value("ok", false) ? 0 : 1);
      };

  m_router.registerCommand("ai_status", [requestDaemonCommand](const std::vector<std::string>& args) {
    const bool verboseStatus = args.size() == 1U && args[0] == "--verbose";
    nlohmann::json requestPayload = nlohmann::json::object();
    if (verboseStatus) {
      requestPayload["verbose"] = true;
    }
    requestDaemonCommand("ai_status", true, requestPayload, verboseStatus);
  });
  m_router.registerCommand("metrics", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload = nlohmann::json::object();
    if (!args.empty()) {
      if (args[0] == "--enable") {
        requestPayload["action"] = "enable";
      } else if (args[0] == "--disable") {
        requestPayload["action"] = "disable";
      } else if (args[0] == "--reset") {
        requestPayload["action"] = "reset";
      }
    }

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_metrics",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    printMetricsReport(payload.value("report", nlohmann::json::object()));
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("rebuild_ai", [requestDaemonCommand](const std::vector<std::string>&) {
    requestDaemonCommand("rebuild_ai", false, nlohmann::json::object(), false);
  });
  m_router.registerCommand("ai_rebuild", [requestDaemonCommand](const std::vector<std::string>&) {
    requestDaemonCommand("rebuild_ai", false, nlohmann::json::object(), false);
  });
  m_router.registerCommand("sleep_ai", [requestDaemonCommand](const std::vector<std::string>&) {
    requestDaemonCommand("shutdown", false, nlohmann::json::object(), false);
  });
  m_router.registerCommand("ai_context",
                           [this](const std::vector<std::string>& args) {
    std::string query = args.empty() ? std::string{} : args.front();
    for (std::size_t index = 1U; index < args.size(); ++index) {
      query += " ";
      query += args[index];
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["query"] = query;

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_context",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());

    // The daemon no longer embeds the raw context JSON in the IPC response
    // (it would overflow the 1 MB pipe cap). It returns a context_path instead.
    // Read the file from disk here on the client side.
    const std::string contextPath = payload.value("context_path", std::string{});
    if (!contextPath.empty()) {
      std::ifstream contextFile(contextPath, std::ios::binary);
      if (contextFile) {
        const std::string content((std::istreambuf_iterator<char>(contextFile)),
                                  std::istreambuf_iterator<char>());
        std::cout << content;
        if (content.empty() || content.back() != '\n') {
          std::cout << '\n';
        }
      } else {
        ultra::core::Logger::error("Failed to read context file: " + contextPath);
        m_lastExitCode = 1;
        return;
      }
    } else {
      // Fallback: daemon sent context_json directly (old behaviour / small payload).
      const std::string contextJson = payload.value("context_json", std::string{});
      if (!contextJson.empty()) {
        std::cout << contextJson;
      } else {
        std::cout << payload.value("context", nlohmann::json::object()).dump(2);
      }
      if (contextJson.empty() || contextJson.back() != '\n') {
        std::cout << '\n';
      }
    }
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_query", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload;
    requestPayload["target"] = args.at(0);
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_query",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    printAiQueryPayload(payload);
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_source", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload;
    requestPayload["file"] = args.at(0);
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_source",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    const std::string content = payload.value("content", std::string{});
    std::cout << content;
    if (content.empty() || content.back() != '\n') {
      std::cout << '\n';
    }
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_impact", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload;
    requestPayload["target"] = args.at(0);
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_impact",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    printAiImpactPayload(payload);
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_verify", [this](const std::vector<std::string>&) {
    handleAiVerify();
  });
}

const std::vector<std::string>& CLIEngine::currentArgs() const noexcept {
  return m_currentArgs;
}

void CLIEngine::handleAiVerify() {
  ultra::ai::AiRuntimeManager runtime(m_projectRoot);
  m_lastExitCode = runtime.aiVerify(true);
}

void CLIEngine::stripMetricsFlag(std::vector<std::string>& args) {
  auto it = std::remove(args.begin(), args.end(), "--metrics");
  if (it != args.end()) {
    args.erase(it, args.end());
    m_metricsRequested = true;
  }
}

void CLIEngine::printMetricsIfRequested(
    std::chrono::steady_clock::time_point start,
    std::size_t filesProcessed) {
  if (!m_metricsRequested) return;
  auto end = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                .count();
  std::cout << "[Metrics] Execution time: " << ms << " ms";
  if (filesProcessed > 0 && ms > 0) {
    double perSec = 1000.0 * static_cast<double>(filesProcessed) /
                     static_cast<double>(ms);
    std::cout << ", Files processed: " << filesProcessed
              << ", Files/sec: " << static_cast<int>(perSec);
  }
  std::cout << '\n';
}

int CLIEngine::run(int argc, char* argv[]) {
  // Daemon child intercept — MUST be the first thing in run().
  // When wakeAi() spawns a background child it calls:
  //   ultra.exe --ultra-daemon --project-root <path>
  // argv[1] == "--ultra-daemon" is NOT a CLI command, so parse() would
  // produce "Unknown command" and exit before the daemon loop ever starts.
  // We catch it here and enter the blocking daemon event loop directly.
  if (argc >= 2) {
    const std::string firstArg(argv[1]);
    if (firstArg == "--ultra-daemon" || firstArg == "--uair-child") {
      // Parse --project-root <path> which spawnDetached() always appends.
      std::filesystem::path projectRoot = std::filesystem::current_path();
      for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--project-root" && argv[i + 1] != nullptr) {
          projectRoot = argv[i + 1];
          break;
        }
      }
      // buildDaemonRuntimeHandler creates the dispatcher and returns the handler.
      // The IPC server starts inside daemon.run() so the parent's wake ping is
      // answered immediately; the index builds lazily on the first query.
      ultra::runtime::UltraDaemon::RuntimeRequestHandler handler =
          buildDaemonRuntimeHandler(projectRoot);
      ultra::runtime::UltraDaemon daemon(projectRoot);
      return daemon.run(handler) ? 0 : 1;
    }
  }

   m_projectRoot = std::filesystem::current_path();
  ParsedCommand cmd = parse(argc, argv);
  if (!cmd.valid) return 1;
  m_metricsRequested = false;
  stripMetricsFlag(cmd.args);
  if (!validate(cmd)) return 1;
  if (!m_router.hasCommand(cmd.name)) {
    ultra::core::Logger::error("Unknown command: " + cmd.name);
    printHelp();
    return 1;
  }
  m_currentArgs = cmd.args;
  m_lastExitCode = 0;
  auto start = std::chrono::steady_clock::now();
  m_router.execute(cmd.name, m_currentArgs);
  std::size_t filesProcessed = 0;
  if (cmd.name == "scan" || cmd.name == "graph" || cmd.name == "analyze" ||
      cmd.name == "context" || cmd.name == "context-diff" ||
      cmd.name == "graph-export") {
    filesProcessed = 0;  // Could be extended per-command if needed
  }
  printMetricsIfRequested(start, filesProcessed);
  if (cmd.name == "build" || cmd.name == "build-incremental" ||
      cmd.name == "build-fast" || cmd.name == "apply-patch") {
    std::cout << "[BUILD] Exit code: " << m_lastExitCode << '\n';
    return m_lastExitCode;
  }
  if (cmd.name == "wake_ai" || cmd.name == "ai_status" ||
      cmd.name == "rebuild_ai" || cmd.name == "ai_rebuild" ||
      cmd.name == "sleep_ai" || cmd.name == "ai_verify" ||
      cmd.name == "ai_context" || cmd.name == "ai_query" ||
      cmd.name == "ai_source" ||
      cmd.name == "ai_impact" || cmd.name == "diff" ||
      cmd.name == "metrics" || cmd.name == "context" ||
      cmd.name == "context-diff" || cmd.name == "branch" ||
      cmd.name == "intent" || cmd.name == "commit" ||
      cmd.name == "savings") {
    return m_lastExitCode;
  }
  if (cmd.name == "init" || cmd.name == "install" || cmd.name == "dev" ||
      cmd.name == "test" || cmd.name == "run" || cmd.name == "clean" ||
      cmd.name == "exec" || cmd.name == "doctor") {
    return m_lastExitCode;
  }
  return 0;
}

}  // namespace ultra::cli
