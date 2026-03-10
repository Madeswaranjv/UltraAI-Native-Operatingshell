#include "ImpactPredictionEngine.h"

#include "../../ai/Hashing.h"
#include "../../ai/SymbolTable.h"
#include "../../core/graph_store/GraphLoader.h"
#include "../../core/graph_store/GraphStore.h"
#include "../../runtime/CPUGovernor.h"
#include "../query/QueryPlanner.h"
//E:\Projects\Ultra\src\engine\impact\ImpactPredictionEngine.cpp
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>
#include <utility>

namespace ultra::engine::impact {

namespace {

ai::RuntimeState normalizeState(ai::RuntimeState state) {
  core::graph_store::GraphLoader::normalizeRuntimeState(state);
  if (state.symbolIndex.empty()) {
    core::graph_store::GraphLoader::rebuildSymbolIndex(state);
  }
  return state;
}

template <typename T>
void sortAndDedupe(std::vector<T>& values) {
  query::QueryPlanner::sortAndDedupe(values);
}

std::string normalizePath(const std::string& value) {
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

struct ScopedGovernorWorkload {
  runtime::CPUGovernor& governor;
  const char* name;
  std::chrono::steady_clock::time_point start;
  std::size_t recommendedThreads{1U};

  explicit ScopedGovernorWorkload(const char* workloadName)
      : governor(runtime::CPUGovernor::instance()),
        name(workloadName),
        start(std::chrono::steady_clock::now()) {
    governor.registerWorkload(name);
    unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads == 0U) {
      hardwareThreads = 4U;
    }
    recommendedThreads = governor.recommendedThreadCount(
        static_cast<std::size_t>(hardwareThreads), name);
  }

  ~ScopedGovernorWorkload() {
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start)
            .count();
    governor.recordExecutionTime(name, elapsedMs);
  }
};

}  // namespace

ImpactPredictionEngine::ImpactPredictionEngine(ai::RuntimeState state,
                                               core::graph_store::GraphStore* graphStore,
                                               const std::uint64_t stateVersion)
    : state_(normalizeState(std::move(state))),
      graphStore_(graphStore),
      queryEngine_(graphStore),
      contextBuilder_(state_, graphStore, stateVersion == 0U ? 1U : stateVersion) {
  const std::uint64_t normalizedVersion = stateVersion == 0U ? 1U : stateVersion;
  queryEngine_.rebuild(state_, normalizedVersion);

  for (const ai::FileRecord& file : state_.files) {
    const std::string normalizedPath = normalizePath(file.path);
    if (normalizedPath.empty()) {
      continue;
    }
    fileIdByPath_[normalizedPath] = file.fileId;
    filePathById_[file.fileId] = normalizedPath;
  }

  for (const auto& [name, node] : state_.symbolIndex) {
    if (!node.definedIn.empty()) {
      definedSymbolsByFile_[normalizePath(node.definedIn)].push_back(name);
    }
    for (const std::string& usedInPath : node.usedInFiles) {
      referencedSymbolsByFile_[normalizePath(usedInPath)].push_back(name);
    }
  }

  for (const ai::SymbolRecord& symbol : state_.symbols) {
    symbolById_[symbol.symbolId] = symbol;
    if (!symbol.name.empty()) {
      symbolsByName_[symbol.name].push_back(symbol);
    }
    if (!isDefinitionSymbol(symbol.symbolType)) {
      continue;
    }

    const auto pathIt = filePathById_.find(symbol.fileId);
    if (pathIt == filePathById_.end()) {
      continue;
    }
    definedSymbolsByFile_[pathIt->second].push_back(symbol.name);
  }

  for (auto& [name, records] : symbolsByName_) {
    (void)name;
    ai::SymbolTable::sortDeterministic(records);
  }
  for (auto& [path, symbols] : definedSymbolsByFile_) {
    (void)path;
    sortAndDedupe(symbols);
  }
  for (auto& [path, symbols] : referencedSymbolsByFile_) {
    (void)path;
    sortAndDedupe(symbols);
  }

  for (const ai::FileDependencyEdge& edge : state_.deps.fileEdges) {
    fileForwardAdj_[edge.fromFileId].push_back(edge.toFileId);
    fileReverseAdj_[edge.toFileId].push_back(edge.fromFileId);
  }
  for (auto& [fileId, neighbors] : fileForwardAdj_) {
    (void)fileId;
    sortAndDedupe(neighbors);
  }
  for (auto& [fileId, neighbors] : fileReverseAdj_) {
    (void)fileId;
    sortAndDedupe(neighbors);
  }

  for (const ai::SymbolDependencyEdge& edge : state_.deps.symbolEdges) {
    symbolForwardAdj_[edge.fromSymbolId].push_back(edge.toSymbolId);
    symbolReverseAdj_[edge.toSymbolId].push_back(edge.fromSymbolId);
  }
  for (auto& [symbolId, neighbors] : symbolForwardAdj_) {
    (void)symbolId;
    sortAndDedupe(neighbors);
  }
  for (auto& [symbolId, neighbors] : symbolReverseAdj_) {
    (void)symbolId;
    sortAndDedupe(neighbors);
  }
}

ImpactPrediction ImpactPredictionEngine::predictSymbolImpact(
    const std::string& symbolName,
    const std::size_t maxDepth) const {
  ScopedGovernorWorkload workload("impact_prediction");
  (void)workload.recommendedThreads;
  return buildPrediction(
      planner_.planSymbolImpact(symbolName, queryEngine_, contextBuilder_, maxDepth));
}

ImpactPrediction ImpactPredictionEngine::predictFileImpact(
    const std::string& filePath,
    const std::size_t maxDepth) const {
  ScopedGovernorWorkload workload("impact_prediction");
  (void)workload.recommendedThreads;
  return buildPrediction(
      planner_.planFileImpact(filePath, queryEngine_, contextBuilder_, maxDepth));
}

SimulationResult ImpactPredictionEngine::simulateSymbolChange(
    const std::string& symbolName,
    const std::size_t maxDepth) const {
  return simulator_.simulateSymbolChange(predictSymbolImpact(symbolName, maxDepth));
}

SimulationResult ImpactPredictionEngine::simulateFileChange(
    const std::string& filePath,
    const std::size_t maxDepth) const {
  return simulator_.simulateFileChange(predictFileImpact(filePath, maxDepth));
}

bool ImpactPredictionEngine::isDefinitionSymbol(
    const ai::SymbolType symbolType) {
  switch (symbolType) {
    case ai::SymbolType::Class:
    case ai::SymbolType::Function:
    case ai::SymbolType::EntryPoint:
    case ai::SymbolType::ReactComponent:
      return true;
    case ai::SymbolType::Unknown:
    case ai::SymbolType::Import:
    case ai::SymbolType::Export:
    default:
      return false;
  }
}

bool ImpactPredictionEngine::isPublicHeaderPath(const std::string& path) {
  const std::string extension = std::filesystem::path(path).extension().string();
  return extension == ".h" || extension == ".hh" || extension == ".hpp" ||
         extension == ".hxx";
}

std::uint64_t ImpactPredictionEngine::deterministicSymbolId(
    const std::string& name,
    const std::string& definedIn) {
  const ai::Sha256Hash digest = ai::sha256OfString(name + "|" + definedIn);
  std::uint64_t symbolId = 0U;
  for (std::size_t index = 0U; index < sizeof(symbolId); ++index) {
    symbolId = (symbolId << 8U) | static_cast<std::uint64_t>(digest[index]);
  }
  return symbolId;
}

ImpactPrediction ImpactPredictionEngine::buildPrediction(
    const ImpactPlan& plan) const {
  ImpactPrediction prediction;
  prediction.targetKind = plan.targetKind;
  prediction.target = plan.target;
  prediction.context = plan.context;
  prediction.files = collectImpactedFiles(plan);
  prediction.symbols = collectImpactedSymbols(plan, prediction.files);
  prediction.affectedFiles = extractAffectedFiles(prediction.files);
  prediction.affectedSymbols = extractAffectedSymbols(prediction.symbols);
  prediction.impactRegion = prediction.affectedFiles;

  std::map<std::string, std::vector<std::string>> symbolsByFile;
  for (const ImpactedSymbol& symbol : prediction.symbols) {
    if (!symbol.definedIn.empty()) {
      symbolsByFile[symbol.definedIn].push_back(symbol.name);
    }
  }
  for (auto& [path, names] : symbolsByFile) {
    (void)path;
    sortAndDedupe(names);
  }
  for (ImpactedFile& file : prediction.files) {
    const auto it = symbolsByFile.find(file.path);
    if (it != symbolsByFile.end()) {
      file.affectedSymbols = it->second;
    }
  }

  prediction.risk = riskEvaluator_.evaluate(plan, prediction.files, prediction.symbols);
  return prediction;
}

std::vector<ImpactedFile> ImpactPredictionEngine::collectImpactedFiles(
    const ImpactPlan& plan) const {
  std::map<std::string, ImpactedFile> filesByPath;
  const auto upsertFile = [&filesByPath](const std::string& rawPath,
                                         const std::size_t depth,
                                         const bool isRoot) {
    const std::string path = normalizePath(rawPath);
    if (path.empty()) {
      return;
    }

    auto [it, inserted] = filesByPath.try_emplace(path);
    ImpactedFile& file = it->second;
    if (inserted) {
      file.path = path;
      file.depth = depth;
      file.isRoot = isRoot;
      return;
    }
    file.depth = std::min(file.depth, depth);
    file.isRoot = file.isRoot || isRoot;
  };

  for (const std::string& rootFile : plan.rootFiles) {
    const std::string resolved =
        normalizePath(contextBuilder_.resolveFilePath(rootFile));
    upsertFile(resolved.empty() ? rootFile : resolved, 0U, true);
  }

  if (plan.targetKind == ImpactTargetKind::Symbol) {
    std::vector<std::string> seedPaths = plan.fileTraversalSeeds;
    sortAndDedupe(seedPaths);
    for (const std::string& seedPath : seedPaths) {
      const std::string resolved =
          normalizePath(contextBuilder_.resolveFilePath(seedPath));
      upsertFile(resolved.empty() ? seedPath : resolved, 1U, false);
    }

    if (plan.fileDepth > 1U) {
      ImpactGraphTraversal::Request<std::uint32_t> request;
      request.startNodes = resolveFileIds(seedPaths);
      request.direction = plan.fileDirection;
      request.maxDepth = plan.fileDepth - 1U;
      request.maxNodes = plan.maxFiles;
      request.includeStartNodes = false;
      request.allowedNodes = resolveFileIds(plan.boundaryFiles);
      const auto traversal =
          ImpactGraphTraversal::traverse(request, fileForwardAdj_, fileReverseAdj_);
      for (const auto& [fileId, traversalDepth] : traversal.depthByNode) {
        const auto pathIt = filePathById_.find(fileId);
        if (pathIt == filePathById_.end()) {
          continue;
        }
        upsertFile(pathIt->second, traversalDepth + 1U, false);
      }
    }
  } else {
    const std::vector<std::string> seedPaths =
        plan.fileTraversalSeeds.empty() ? plan.rootFiles : plan.fileTraversalSeeds;
    if (plan.fileDepth > 0U) {
      ImpactGraphTraversal::Request<std::uint32_t> request;
      request.startNodes = resolveFileIds(seedPaths);
      request.direction = plan.fileDirection;
      request.maxDepth = plan.fileDepth;
      request.maxNodes = plan.maxFiles;
      request.includeStartNodes = false;
      request.allowedNodes = resolveFileIds(plan.boundaryFiles);
      const auto traversal =
          ImpactGraphTraversal::traverse(request, fileForwardAdj_, fileReverseAdj_);
      for (const auto& [fileId, traversalDepth] : traversal.depthByNode) {
        const auto pathIt = filePathById_.find(fileId);
        if (pathIt == filePathById_.end()) {
          continue;
        }
        upsertFile(pathIt->second, traversalDepth, false);
      }
    }
  }

  std::vector<ImpactedFile> files;
  files.reserve(filesByPath.size());
  for (auto& [path, file] : filesByPath) {
    (void)path;
    files.push_back(std::move(file));
  }
  std::sort(files.begin(), files.end(),
            [](const ImpactedFile& left, const ImpactedFile& right) {
              return left.path < right.path;
            });
  return files;
}

std::vector<ImpactedSymbol> ImpactPredictionEngine::collectImpactedSymbols(
    const ImpactPlan& plan,
    const std::vector<ImpactedFile>& files) const {
  std::map<std::string, ImpactedSymbol> symbolsByKey;
  const auto upsertSymbol = [&symbolsByKey](ImpactedSymbol symbol) {
    if (symbol.name.empty()) {
      return;
    }

    const std::string key = std::to_string(symbol.symbolId) + "|" + symbol.name +
                            "|" + symbol.definedIn;
    auto [it, inserted] = symbolsByKey.try_emplace(key, std::move(symbol));
    if (inserted) {
      return;
    }

    ImpactedSymbol& existing = it->second;
    existing.depth = std::min(existing.depth, symbol.depth);
    existing.isRoot = existing.isRoot || symbol.isRoot;
    existing.publicApi = existing.publicApi || symbol.publicApi;
    existing.centrality = std::max(existing.centrality, symbol.centrality);
    if (existing.definedIn.empty()) {
      existing.definedIn = symbol.definedIn;
    }
    if ((existing.lineNumber == 0U && symbol.lineNumber != 0U) ||
        (symbol.lineNumber != 0U && symbol.lineNumber < existing.lineNumber)) {
      existing.lineNumber = symbol.lineNumber;
    }
  };

  for (const std::string& symbolName : plan.rootSymbols) {
    upsertSymbol(buildImpactedSymbol(symbolName, 0U, true));
  }

  if (plan.symbolDepth > 0U) {
    ImpactGraphTraversal::Request<std::uint64_t> request;
    request.startNodes =
        resolveSymbolIds(plan.symbolTraversalSeeds.empty() ? plan.rootSymbols
                                                           : plan.symbolTraversalSeeds);
    request.direction = plan.symbolDirection;
    request.maxDepth = plan.symbolDepth;
    request.maxNodes = plan.maxSymbols;
    request.includeStartNodes = false;
    request.allowedNodes = resolveSymbolIds(plan.boundarySymbols);
    const auto traversal = ImpactGraphTraversal::traverse(
        request, symbolForwardAdj_, symbolReverseAdj_);
    for (const auto& [symbolId, traversalDepth] : traversal.depthByNode) {
      const auto symbolIt = symbolById_.find(symbolId);
      if (symbolIt == symbolById_.end()) {
        continue;
      }
      const auto filePathIt = filePathById_.find(symbolIt->second.fileId);
      upsertSymbol(buildImpactedSymbol(
          symbolIt->second.name, traversalDepth, false,
          filePathIt == filePathById_.end() ? std::string{} : filePathIt->second));
    }
  }

  for (const ImpactedFile& file : files) {
    for (const std::string& symbolName : definedSymbolsForFile(file.path)) {
      const bool isRoot =
          file.isRoot &&
          std::find(plan.rootSymbols.begin(), plan.rootSymbols.end(), symbolName) !=
              plan.rootSymbols.end();
      upsertSymbol(
          buildImpactedSymbol(symbolName, file.depth, isRoot, file.path));
    }
  }

  std::vector<ImpactedSymbol> symbols;
  symbols.reserve(symbolsByKey.size());
  for (auto& [key, symbol] : symbolsByKey) {
    (void)key;
    symbols.push_back(std::move(symbol));
  }
  std::sort(symbols.begin(), symbols.end(),
            [](const ImpactedSymbol& left, const ImpactedSymbol& right) {
              if (left.symbolId != right.symbolId) {
                return left.symbolId < right.symbolId;
              }
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.definedIn < right.definedIn;
            });
  return symbols;
}

std::vector<std::uint32_t> ImpactPredictionEngine::resolveFileIds(
    const std::vector<std::string>& paths) const {
  std::vector<std::uint32_t> fileIds;
  fileIds.reserve(paths.size());
  for (const std::string& rawPath : paths) {
    const std::string resolved =
        normalizePath(contextBuilder_.resolveFilePath(rawPath));
    const std::string path = resolved.empty() ? normalizePath(rawPath) : resolved;
    const auto localIt = fileIdByPath_.find(path);
    if (localIt != fileIdByPath_.end()) {
      fileIds.push_back(localIt->second);
      continue;
    }
    if (graphStore_ != nullptr) {
      const auto fileId = graphStore_->fileIdForPath(path);
      if (fileId.has_value()) {
        fileIds.push_back(*fileId);
      }
    }
  }
  sortAndDedupe(fileIds);
  return fileIds;
}

std::vector<std::uint64_t> ImpactPredictionEngine::resolveSymbolIds(
    const std::vector<std::string>& symbolNames) const {
  std::vector<std::uint64_t> symbolIds;
  for (const std::string& symbolName : symbolNames) {
    const auto definitions = queryEngine_.findDefinition(symbolName);
    for (const query::SymbolDefinition& definition : definitions) {
      if (definition.symbolId != 0U) {
        symbolIds.push_back(definition.symbolId);
      }
    }

    const auto recordsIt = symbolsByName_.find(symbolName);
    if (recordsIt == symbolsByName_.end()) {
      continue;
    }
    for (const ai::SymbolRecord& record : recordsIt->second) {
      if (record.symbolId != 0U) {
        symbolIds.push_back(record.symbolId);
      }
    }
  }
  sortAndDedupe(symbolIds);
  return symbolIds;
}

std::vector<std::string> ImpactPredictionEngine::definedSymbolsForFile(
    const std::string& rawFilePath) const {
  const std::string filePath = normalizePath(rawFilePath);
  const auto definitionsIt = definedSymbolsByFile_.find(filePath);
  if (definitionsIt != definedSymbolsByFile_.end() &&
      !definitionsIt->second.empty()) {
    return definitionsIt->second;
  }

  const auto referencedIt = referencedSymbolsByFile_.find(filePath);
  if (referencedIt != referencedSymbolsByFile_.end()) {
    return referencedIt->second;
  }
  return {};
}

ImpactedSymbol ImpactPredictionEngine::buildImpactedSymbol(
    const std::string& symbolName,
    const std::size_t depth,
    const bool isRoot,
    const std::string& preferredFilePath) const {
  ImpactedSymbol impacted;
  impacted.name = symbolName;
  impacted.depth = depth;
  impacted.isRoot = isRoot;

  const auto symbolNodeIt = state_.symbolIndex.find(symbolName);
  if (symbolNodeIt != state_.symbolIndex.end()) {
    impacted.definedIn = normalizePath(symbolNodeIt->second.definedIn);
    impacted.centrality = symbolNodeIt->second.centrality;
  }

  const auto recordsIt = symbolsByName_.find(symbolName);
  if (recordsIt != symbolsByName_.end() && !recordsIt->second.empty()) {
    const ai::SymbolRecord* selected = nullptr;
    const std::string normalizedPreferred = normalizePath(preferredFilePath);

    const auto matchesPreferredPath = [this, &normalizedPreferred](
                                          const ai::SymbolRecord& record) {
      const auto pathIt = filePathById_.find(record.fileId);
      return pathIt != filePathById_.end() &&
             pathIt->second == normalizedPreferred;
    };

    if (!normalizedPreferred.empty()) {
      for (const ai::SymbolRecord& record : recordsIt->second) {
        if (isDefinitionSymbol(record.symbolType) && matchesPreferredPath(record)) {
          selected = &record;
          break;
        }
      }
      if (selected == nullptr) {
        for (const ai::SymbolRecord& record : recordsIt->second) {
          if (matchesPreferredPath(record)) {
            selected = &record;
            break;
          }
        }
      }
    }
    if (selected == nullptr) {
      for (const ai::SymbolRecord& record : recordsIt->second) {
        if (isDefinitionSymbol(record.symbolType)) {
          selected = &record;
          break;
        }
      }
    }
    if (selected == nullptr) {
      selected = &recordsIt->second.front();
    }

    impacted.symbolId = selected->symbolId;
    impacted.lineNumber = selected->lineNumber;
    const auto pathIt = filePathById_.find(selected->fileId);
    if (pathIt != filePathById_.end()) {
      impacted.definedIn = pathIt->second;
    } else if (!normalizedPreferred.empty()) {
      impacted.definedIn = normalizedPreferred;
    }
    impacted.publicApi =
        selected->visibility == ai::Visibility::Public ||
        isPublicHeaderPath(impacted.definedIn);

    if (graphStore_ != nullptr) {
      const auto stableId = graphStore_->symbolIdForDeterministicKey(*selected);
      if (stableId.has_value()) {
        impacted.symbolId = *stableId;
      }
    }
    if (impacted.symbolId == 0U) {
      impacted.symbolId =
          deterministicSymbolId(symbolName, impacted.definedIn);
    }
    return impacted;
  }

  if (!preferredFilePath.empty()) {
    impacted.definedIn = normalizePath(preferredFilePath);
  }
  impacted.publicApi = isPublicHeaderPath(impacted.definedIn);
  impacted.symbolId = deterministicSymbolId(symbolName, impacted.definedIn);
  return impacted;
}

std::vector<std::string> ImpactPredictionEngine::extractAffectedFiles(
    const std::vector<ImpactedFile>& files) const {
  std::vector<std::string> affectedFiles;
  affectedFiles.reserve(files.size());
  for (const ImpactedFile& file : files) {
    if (!file.path.empty()) {
      affectedFiles.push_back(file.path);
    }
  }
  sortAndDedupe(affectedFiles);
  return affectedFiles;
}

std::vector<std::string> ImpactPredictionEngine::extractAffectedSymbols(
    const std::vector<ImpactedSymbol>& symbols) const {
  std::vector<std::string> affectedSymbols;
  affectedSymbols.reserve(symbols.size());
  for (const ImpactedSymbol& symbol : symbols) {
    if (!symbol.name.empty()) {
      affectedSymbols.push_back(symbol.name);
    }
  }
  sortAndDedupe(affectedSymbols);
  return affectedSymbols;
}

}  // namespace ultra::engine::impact
