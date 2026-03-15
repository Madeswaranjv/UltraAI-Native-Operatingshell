#include "ContextBuilder.h"

#include "../../ai/SymbolTable.h"
#include "../../core/graph_store/GraphLoader.h"
#include "../context_compression/ContextCompressor.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <utility>

namespace ultra::engine::context {

namespace {

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

std::string contextKindToString(const ContextKind kind) {
  switch (kind) {
    case ContextKind::Symbol:
      return "symbol_context";
    case ContextKind::File:
      return "file_context";
    case ContextKind::Impact:
      return "impact_context";
  }
  return "symbol_context";
}

double roundFixed3(const double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::round(value * 1000.0) / 1000.0;
}

bool eraseField(nlohmann::ordered_json& object, const char* key) {
  if (!object.is_object() || !object.contains(key)) {
    return false;
  }
  object.erase(key);
  return true;
}

bool clearArrayField(nlohmann::ordered_json& object, const char* key) {
  if (!object.is_object() || !object.contains(key) || !object[key].is_array() ||
      object[key].empty()) {
    return false;
  }
  object[key] = nlohmann::ordered_json::array();
  return true;
}

bool trimDefinitions(nlohmann::ordered_json& object, const bool allowErase) {
  if (!object.is_object() || !object.contains("definitions") ||
      !object["definitions"].is_array()) {
    return false;
  }

  auto& definitions = object["definitions"];
  if (definitions.size() > 1U) {
    while (definitions.size() > 1U) {
      definitions.erase(definitions.end() - 1);
    }
    return true;
  }

  if (allowErase && !definitions.empty()) {
    object.erase("definitions");
    return true;
  }
  return false;
}

bool compactPayloadItems(nlohmann::ordered_json& items,
                         const bool rootItems,
                         const std::vector<const char*>& fieldOrder,
                         const bool trimDefinitionsField = false,
                         const bool eraseDefinitions = false) {
  if (!items.is_array()) {
    return false;
  }

  for (auto& item : items) {
    if (!item.is_object() || item.value("is_root", false) != rootItems) {
      continue;
    }

    if (trimDefinitionsField && trimDefinitions(item, eraseDefinitions && rootItems)) {
      return true;
    }
    for (const char* field : fieldOrder) {
      if (eraseField(item, field)) {
        return true;
      }
    }
    if (trimDefinitionsField && trimDefinitions(item, eraseDefinitions)) {
      return true;
    }
  }

  return false;
}

bool compactContextSlice(ContextSlice& slice,
                         const ContextKind kind,
                         const TokenBudgetManager& budgetManager) {
  // Refresh token count without touching the truncated flag.
  // truncated is only set to true inside the loop, when content is
  // actually erased from the payload due to budget pressure.
  auto refresh = [&slice, &budgetManager]() {
    slice.json = slice.payload.dump();
    slice.estimatedTokens = budgetManager.estimateTextTokens(slice.json);
    slice.payload["metadata"]["estimatedTokens"] = slice.estimatedTokens;
  };

  refresh();
  while (!budgetManager.fits(slice.estimatedTokens)) {
    bool changed = false;

    changed = clearArrayField(slice.payload, "impact_region");
    if (!changed && kind == ContextKind::File && slice.payload.contains("nodes") &&
        slice.payload["nodes"].is_array() && !slice.payload["nodes"].empty()) {
      slice.payload["nodes"] = nlohmann::ordered_json::array();
      slice.includedNodes.clear();
      changed = true;
    }
    if (!changed && kind != ContextKind::File && slice.payload.contains("files") &&
        slice.payload["files"].is_array() && !slice.payload["files"].empty()) {
      slice.payload["files"] = nlohmann::ordered_json::array();
      changed = true;
    }
    if (!changed) {
      changed = compactPayloadItems(slice.payload["files"], false,
                                    {"relevant_symbols", "dependencies", "weight"});
    }
    if (!changed) {
      changed = compactPayloadItems(
          slice.payload["nodes"], false,
          {"impact_files", "references", "dependencies", "weight", "centrality"},
          true, true);
    }
    if (!changed) {
      changed = compactPayloadItems(
          slice.payload["files"], true,
          {"relevant_symbols", "dependencies", "weight", "distance", "is_root"});
    }
    if (!changed) {
      changed = compactPayloadItems(
          slice.payload["nodes"], true,
          {"impact_files", "references", "dependencies", "weight", "centrality",
           "distance", "is_root", "defined_in"},
          true, true);
    }
    if (!changed && kind == ContextKind::File && slice.payload.contains("files") &&
        slice.payload["files"].is_array() && !slice.payload["files"].empty()) {
      slice.payload["files"] = nlohmann::ordered_json::array();
      changed = true;
    }
    if (!changed) {
      changed = eraseField(slice.payload["metadata"], "rawEstimatedTokens");
    }
    if (!changed) {
      changed = eraseField(slice.payload["metadata"], "candidateFileCount");
    }
    if (!changed) {
      changed = eraseField(slice.payload["metadata"], "candidateNodeCount");
    }
    if (!changed) {
      break;
    }

    // Only mark truncated when we actually removed content.
    slice.payload["metadata"]["truncated"] = true;
    refresh();
  }

  refresh();
  return budgetManager.fits(slice.estimatedTokens);
}

template <typename RankedCandidate>
std::vector<RankedCandidate> selectedOnly(
    const std::vector<RankedCandidate>& ranked) {
  std::vector<RankedCandidate> selected;
  for (const RankedCandidate& candidate : ranked) {
    if (candidate.selected) {
      selected.push_back(candidate);
    }
  }
  return selected;
}

}  // namespace

ContextBuilder::ContextBuilder(ai::RuntimeState state,
                               core::graph_store::GraphStore* graphStore,
                               const std::uint64_t stateVersion,
                               const ContextBuilderOptions options)
    : state_(std::move(state)),
      graphStore_(graphStore),
      queryEngine_(graphStore),
      options_(options) {
  core::graph_store::GraphLoader::normalizeRuntimeState(state_);
  if (state_.symbolIndex.empty()) {
    core::graph_store::GraphLoader::rebuildSymbolIndex(state_);
  }
  queryEngine_.rebuild(state_, stateVersion == 0U ? 1U : stateVersion);

  for (const ai::FileRecord& file : state_.files) {
    canonicalFilePaths_[normalizePathToken(file.path)] = file.path;
  }

  for (const auto& [name, node] : state_.symbolIndex) {
    if (!node.definedIn.empty()) {
      definedSymbolsByFile_[node.definedIn].push_back(name);
    }
    for (const std::string& usedInPath : node.usedInFiles) {
      if (!usedInPath.empty()) {
        referencedSymbolsByFile_[usedInPath].push_back(name);
      }
    }
  }
  for (auto& [path, names] : definedSymbolsByFile_) {
    (void)path;
    ContextPruner::collapseStrings(names);
  }
  for (auto& [path, names] : referencedSymbolsByFile_) {
    (void)path;
    ContextPruner::collapseStrings(names);
  }

  for (const ai::SymbolRecord& symbol : state_.symbols) {
    if (!symbol.name.empty()) {
      symbolsByName_[symbol.name].push_back(symbol);
    }
  }
  for (auto& [name, symbols] : symbolsByName_) {
    (void)name;
    ai::SymbolTable::sortDeterministic(symbols);
  }
}

bool ContextBuilder::hasSymbol(const std::string& symbolName) const {
  return state_.symbolIndex.find(symbolName) != state_.symbolIndex.end() ||
         !queryEngine_.findDefinition(symbolName).empty();
}

std::string ContextBuilder::resolveFilePath(const std::string& filePath) const {
  if (filePath.empty()) {
    return {};
  }

  const std::string normalized = normalizePathToken(filePath);
  const auto directIt = canonicalFilePaths_.find(normalized);
  if (directIt != canonicalFilePaths_.end()) {
    return directIt->second;
  }

  std::string uniqueSuffixMatch;
  for (const auto& [canonicalKey, canonicalPath] : canonicalFilePaths_) {
    if (normalized.empty() || normalized.size() >= canonicalKey.size()) {
      continue;
    }
    const std::size_t offset = canonicalKey.size() - normalized.size();
    if (canonicalKey.compare(offset, normalized.size(), normalized) != 0) {
      continue;
    }
    if (offset > 0U && canonicalKey[offset - 1U] != '/') {
      continue;
    }
    if (uniqueSuffixMatch.empty()) {
      uniqueSuffixMatch = canonicalPath;
    } else if (uniqueSuffixMatch != canonicalPath) {
      return {};
    }
  }

  return uniqueSuffixMatch;
}

ContextSlice ContextBuilder::buildSymbolContext(
    const std::string& symbolName,
    const std::size_t tokenBudget,
    const RankingWeights& weights,
    const std::size_t impactDepth) const {
  ContextRequest request;
  request.kind = ContextKind::Symbol;
  request.target = symbolName;
  request.tokenBudget = tokenBudget;
  request.impactDepth = impactDepth;
  request.weights = weights;
  return buildContext(planner_.planSymbolContext(request, queryEngine_));
}

ContextSlice ContextBuilder::buildFileContext(
    const std::string& filePath,
    const std::size_t tokenBudget,
    const RankingWeights& weights,
    const std::size_t impactDepth) const {
  ContextRequest request;
  request.kind = ContextKind::File;
  request.target = filePath;
  request.tokenBudget = tokenBudget;
  request.impactDepth = impactDepth;
  request.weights = weights;
  const std::string resolvedPath = resolveFilePath(filePath);
  return buildContext(
      planner_.planFileContext(request, queryEngine_, state_, resolvedPath));
}

ContextSlice ContextBuilder::buildImpactContext(
    const std::string& symbolName,
    const std::size_t tokenBudget,
    const RankingWeights& weights,
    const std::size_t impactDepth) const {
  ContextRequest request;
  request.kind = ContextKind::Impact;
  request.target = symbolName;
  request.tokenBudget = tokenBudget;
  request.impactDepth = impactDepth;
  request.weights = weights;
  return buildContext(planner_.planImpactContext(request, queryEngine_));
}

ContextSlice ContextBuilder::buildContext(const ContextPlan& plan) const {
  const TokenBudgetManager budgetManager(plan.request.tokenBudget);
  const std::vector<SymbolContextCandidate> symbolCandidates =
      buildSymbolCandidates(plan);
  const std::vector<FileContextCandidate> fileCandidates =
      buildFileCandidates(plan);
  std::vector<RankedSymbolCandidate> rankedSymbols =
      ranker_.rankSymbols(symbolCandidates, plan.request);
  std::vector<RankedFileCandidate> rankedFiles =
      ranker_.rankFiles(fileCandidates, plan.request);

  ContextSlice fullSlice =
      buildSlice(plan, rankedSymbols, rankedFiles, budgetManager, 0U);
  fullSlice.rawEstimatedTokens = fullSlice.estimatedTokens;
  if (budgetManager.fits(fullSlice.estimatedTokens)) {
    return maybeCompressSlice(plan, budgetManager, std::move(fullSlice));
  }

  while (true) {
    const auto decision =
        pruner_.selectNextCandidate(rankedSymbols, rankedFiles);
    if (!decision.has_value()) {
      break;
    }

    if (decision->kind == ContextPruner::PruneDecision::Kind::Symbol) {
      rankedSymbols[decision->index].selected = false;
    } else {
      rankedFiles[decision->index].selected = false;
    }

    ContextSlice candidateSlice = buildSlice(
        plan, rankedSymbols, rankedFiles, budgetManager, fullSlice.estimatedTokens);
    candidateSlice.rawEstimatedTokens = fullSlice.estimatedTokens;
    if (budgetManager.fits(candidateSlice.estimatedTokens)) {
      return maybeCompressSlice(plan, budgetManager, std::move(candidateSlice));
    }
  }

  ContextSlice minimalSlice =
      buildSlice(plan, rankedSymbols, rankedFiles, budgetManager, fullSlice.estimatedTokens);
  minimalSlice.rawEstimatedTokens = fullSlice.estimatedTokens;
  if (!budgetManager.fits(minimalSlice.estimatedTokens) &&
      !compactContextSlice(minimalSlice, plan.request.kind, budgetManager)) {
    throw std::runtime_error(
        "Token budget too small for deterministic context envelope.");
  }
  return maybeCompressSlice(plan, budgetManager, std::move(minimalSlice));
}

ContextSlice ContextBuilder::maybeCompressSlice(
    const ContextPlan& plan,
    const TokenBudgetManager& budgetManager,
    ContextSlice slice) const {
  if (!options_.enableCompression || options_.graphSnapshot == nullptr) {
    return slice;
  }

  try {
    context_compression::ContextCompressor compressor;
    ContextSlice compressed =
        compressor.compressContext(*options_.graphSnapshot,
                                   plan,
                                   queryEngine_,
                                   slice,
                                   budgetManager.budget());
    if (!budgetManager.fits(compressed.estimatedTokens)) {
      return slice;
    }
    if (compressed.estimatedTokens <= slice.estimatedTokens) {
      return compressed;
    }
    return slice;
  } catch (...) {
    return slice;
  }
}

std::vector<SymbolContextCandidate> ContextBuilder::buildSymbolCandidates(
    const ContextPlan& plan) const {
  std::map<std::string, SymbolContextCandidate> candidatesByName;

  const auto addCandidate = [&](const std::string& name,
                                const bool isRoot,
                                const std::size_t distance,
                                std::vector<std::string> impactFiles) {
    if (name.empty()) {
      return;
    }

    const auto symbolNodeIt = state_.symbolIndex.find(name);
    const auto definitions = queryEngine_.findDefinition(name);
    const auto references = queryEngine_.findReferences(name);
    const auto dependencies = queryEngine_.findSymbolDependencies(name);
    if (symbolNodeIt == state_.symbolIndex.end() && definitions.empty() &&
        references.empty() && dependencies.empty()) {
      return;
    }

    ContextPruner::collapseStrings(impactFiles);
    auto [it, inserted] = candidatesByName.try_emplace(name);
    SymbolContextCandidate& candidate = it->second;
    if (inserted) {
      candidate.name = name;
      candidate.symbolId = primarySymbolId(name);
      candidate.definitions = definitions;
      candidate.references = references;
      candidate.dependencies = dependencies;
      candidate.impactFiles = std::move(impactFiles);
      if (symbolNodeIt != state_.symbolIndex.end()) {
        candidate.definedIn = symbolNodeIt->second.definedIn;
        candidate.weight = symbolNodeIt->second.weight;
        candidate.centrality = symbolNodeIt->second.centrality;
      } else if (!definitions.empty()) {
        candidate.definedIn = definitions.front().filePath;
      }
      candidate.distance = distance;
      candidate.isRoot = isRoot;
    } else {
      candidate.isRoot = candidate.isRoot || isRoot;
      candidate.distance = std::min(candidate.distance, distance);
      candidate.impactFiles.insert(candidate.impactFiles.end(), impactFiles.begin(),
                                   impactFiles.end());
      ContextPruner::collapseStrings(candidate.impactFiles);
    }
    ContextPruner::collapseDefinitions(candidate.definitions);
    ContextPruner::collapseStrings(candidate.references);
    ContextPruner::collapseStrings(candidate.dependencies);
  };

  for (const std::string& root : plan.rootSymbols) {
    addCandidate(root, true, 0U,
                 plan.request.kind == ContextKind::Impact
                     ? plan.impactFiles
                     : queryEngine_.findImpactRegion(
                           root, std::max<std::size_t>(1U, plan.request.impactDepth)));
  }
  for (const std::string& dependency : plan.symbolDependencies) {
    addCandidate(dependency, false, 1U,
                 queryEngine_.findImpactRegion(dependency, 1U));
  }
  if (plan.request.kind == ContextKind::File) {
    for (const std::string& symbolName : plan.rootSymbols) {
      addCandidate(symbolName, false, 1U,
                   queryEngine_.findImpactRegion(symbolName, 1U));
    }
  }
  if (plan.request.kind == ContextKind::Impact) {
    for (const std::string& filePath : plan.impactFiles) {
      const auto definedIt = definedSymbolsByFile_.find(filePath);
      if (definedIt == definedSymbolsByFile_.end()) {
        continue;
      }
      for (const std::string& symbolName : definedIt->second) {
        addCandidate(symbolName, false, 2U, {});
      }
    }
  }

  std::vector<SymbolContextCandidate> candidates;
  candidates.reserve(candidatesByName.size());
  for (auto& [name, candidate] : candidatesByName) {
    (void)name;
    candidates.push_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const SymbolContextCandidate& left,
               const SymbolContextCandidate& right) {
              if (left.symbolId != right.symbolId) {
                return left.symbolId < right.symbolId;
              }
              return left.name < right.name;
            });
  return candidates;
}

std::vector<FileContextCandidate> ContextBuilder::buildFileCandidates(
    const ContextPlan& plan) const {
  std::map<std::string, FileContextCandidate> candidatesByPath;

  const auto addCandidate = [&](const std::string& filePath,
                                const bool isRoot,
                                const std::size_t distance) {
    if (filePath.empty()) {
      return;
    }
    auto [it, inserted] = candidatesByPath.try_emplace(filePath);
    FileContextCandidate& candidate = it->second;
    if (inserted) {
      candidate.path = filePath;
      candidate.dependencies = queryEngine_.findFileDependencies(filePath);
      candidate.relevantSymbols = relevantSymbolsForFile(filePath);
      candidate.distance = distance;
      candidate.isRoot = isRoot;
    } else {
      candidate.distance = std::min(candidate.distance, distance);
      candidate.isRoot = candidate.isRoot || isRoot;
      std::vector<std::string> relevant = relevantSymbolsForFile(filePath);
      candidate.relevantSymbols.insert(candidate.relevantSymbols.end(),
                                       relevant.begin(), relevant.end());
    }
    ContextPruner::collapseStrings(candidate.dependencies);
    ContextPruner::collapseStrings(candidate.relevantSymbols);
  };

  for (const std::string& rootFile : plan.rootFiles) {
    addCandidate(rootFile, true, 0U);
  }
  for (const std::string& dependency : plan.fileDependencies) {
    addCandidate(dependency, false, 1U);
  }
  for (const std::string& impactFile : plan.impactFiles) {
    const bool isRoot =
        std::find(plan.rootFiles.begin(), plan.rootFiles.end(), impactFile) !=
        plan.rootFiles.end();
    addCandidate(impactFile, isRoot, isRoot ? 0U : 2U);
  }

  std::vector<FileContextCandidate> candidates;
  candidates.reserve(candidatesByPath.size());
  for (auto& [path, candidate] : candidatesByPath) {
    (void)path;
    candidates.push_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const FileContextCandidate& left,
               const FileContextCandidate& right) {
              return left.path < right.path;
            });
  return candidates;
}

ContextSlice ContextBuilder::buildSlice(
    const ContextPlan& plan,
    const std::vector<RankedSymbolCandidate>& rankedSymbols,
    const std::vector<RankedFileCandidate>& rankedFiles,
    const TokenBudgetManager& budgetManager,
    const std::size_t rawEstimatedTokens) const {
  std::vector<RankedSymbolCandidate> selectedSymbols = selectedOnly(rankedSymbols);
  std::sort(selectedSymbols.begin(), selectedSymbols.end(),
            [](const RankedSymbolCandidate& left,
               const RankedSymbolCandidate& right) {
              if (left.candidate.symbolId != right.candidate.symbolId) {
                return left.candidate.symbolId < right.candidate.symbolId;
              }
              return left.candidate.name < right.candidate.name;
            });

  std::vector<RankedFileCandidate> selectedFiles = selectedOnly(rankedFiles);
  std::sort(selectedFiles.begin(), selectedFiles.end(),
            [](const RankedFileCandidate& left,
               const RankedFileCandidate& right) {
              return left.candidate.path < right.candidate.path;
            });

  nlohmann::ordered_json payload;
  payload["kind"] = contextKindToString(plan.request.kind);
  payload["target"] = plan.request.target;

  nlohmann::ordered_json nodes = nlohmann::ordered_json::array();
  std::vector<std::uint64_t> includedNodes;
  includedNodes.reserve(selectedSymbols.size());
  for (const RankedSymbolCandidate& ranked : selectedSymbols) {
    const SymbolContextCandidate& candidate = ranked.candidate;
    nlohmann::ordered_json node;
    node["centrality"] = roundFixed3(candidate.centrality);
    node["defined_in"] = candidate.definedIn;
    nlohmann::ordered_json definitions = nlohmann::ordered_json::array();
    for (const query::SymbolDefinition& definition : candidate.definitions) {
      nlohmann::ordered_json item;
      item["file_id"] = definition.fileId;
      item["file_path"] = definition.filePath;
      item["line_number"] = definition.lineNumber;
      item["signature"] = definition.signature;
      item["symbol_id"] = definition.symbolId;
      definitions.push_back(std::move(item));
    }
    node["definitions"] = std::move(definitions);
    node["dependencies"] = candidate.dependencies;
    node["distance"] = candidate.distance;
    node["id"] = candidate.symbolId;
    node["impact_files"] = candidate.impactFiles;
    node["is_root"] = candidate.isRoot;
    node["kind"] = "symbol";
    node["name"] = candidate.name;
    node["references"] = candidate.references;
    node["weight"] = roundFixed3(
        static_cast<double>(ranked.scoreMicros) / 1000000.0);
    nodes.push_back(std::move(node));
    includedNodes.push_back(candidate.symbolId);
  }
  payload["nodes"] = std::move(nodes);

  nlohmann::ordered_json files = nlohmann::ordered_json::array();
  std::vector<std::string> impactRegion;
  for (const RankedFileCandidate& ranked : selectedFiles) {
    const FileContextCandidate& candidate = ranked.candidate;
    nlohmann::ordered_json file;
    file["dependencies"] = candidate.dependencies;
    file["distance"] = candidate.distance;
    file["is_root"] = candidate.isRoot;
    file["kind"] = "file";
    file["path"] = candidate.path;
    file["relevant_symbols"] = candidate.relevantSymbols;
    file["weight"] = roundFixed3(
        static_cast<double>(ranked.scoreMicros) / 1000000.0);
    files.push_back(std::move(file));
    impactRegion.push_back(candidate.path);
  }
  ContextPruner::collapseStrings(impactRegion);
  payload["files"] = std::move(files);
  payload["impact_region"] = impactRegion;

  nlohmann::ordered_json metadata;
  metadata["candidateFileCount"] = rankedFiles.size();
  metadata["candidateNodeCount"] = rankedSymbols.size();
  metadata["estimatedTokens"] = 0U;
  metadata["impactDepth"] = std::max<std::size_t>(1U, plan.request.impactDepth);
  metadata["rawEstimatedTokens"] = rawEstimatedTokens;
  metadata["selectedFileCount"] = selectedFiles.size();
  metadata["selectedNodeCount"] = selectedSymbols.size();
  metadata["tokenBudget"] = budgetManager.budget();
  // truncated = false here: candidates may be deselected by the ranker/pruner
  // for relevance reasons even when the slice fits comfortably inside the
  // budget. "truncated" means budget-driven content removal, not ranking-driven
  // selection. compactContextSlice (and compactCompressedSlice) set it to true
  // only when they actually erase fields from the JSON payload.
  metadata["truncated"] = false;
  payload["metadata"] = metadata;

  ContextSlice slice;
  slice.includedNodes = std::move(includedNodes);
  slice.payload = std::move(payload);
  slice.candidateSymbolCount = rankedSymbols.size();
  slice.candidateFileCount = rankedFiles.size();
  slice.rawEstimatedTokens = rawEstimatedTokens;

  std::size_t estimated = budgetManager.estimatePayloadTokens(slice.payload);
  slice.payload["metadata"]["estimatedTokens"] = estimated;
  slice.payload["metadata"]["rawEstimatedTokens"] =
      rawEstimatedTokens == 0U ? estimated : rawEstimatedTokens;
  slice.json = slice.payload.dump();
  slice.estimatedTokens = budgetManager.estimateTextTokens(slice.json);
  if (slice.estimatedTokens != estimated) {
    slice.payload["metadata"]["estimatedTokens"] = slice.estimatedTokens;
    slice.json = slice.payload.dump();
    slice.estimatedTokens = budgetManager.estimateTextTokens(slice.json);
  }
  return slice;
}

std::uint64_t ContextBuilder::primarySymbolId(
    const std::string& symbolName) const {
  const auto definitions = queryEngine_.findDefinition(symbolName);
  if (!definitions.empty()) {
    return definitions.front().symbolId;
  }

  const auto recordsIt = symbolsByName_.find(symbolName);
  if (recordsIt != symbolsByName_.end() && !recordsIt->second.empty()) {
    return recordsIt->second.front().symbolId;
  }

  return 0U;
}

std::vector<std::string> ContextBuilder::relevantSymbolsForFile(
    const std::string& filePath) const {
  std::vector<std::string> names;
  const auto definedIt = definedSymbolsByFile_.find(filePath);
  if (definedIt != definedSymbolsByFile_.end()) {
    names.insert(names.end(), definedIt->second.begin(), definedIt->second.end());
  }
  const auto referencedIt = referencedSymbolsByFile_.find(filePath);
  if (referencedIt != referencedSymbolsByFile_.end()) {
    names.insert(names.end(), referencedIt->second.begin(),
                 referencedIt->second.end());
  }
  ContextPruner::collapseStrings(names);
  return names;
}

}  // namespace ultra::engine::context