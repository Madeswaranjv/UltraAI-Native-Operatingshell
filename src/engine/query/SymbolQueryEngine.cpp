#include "SymbolQueryEngine.h"

#include "../../core/graph_store/GraphLoader.h"
#include "../../core/graph_store/GraphStore.h"

#include <algorithm>
#include <set>
#include <utility>

namespace ultra::engine::query {

namespace {

bool symbolDefinitionLess(const SymbolDefinition& left,
                          const SymbolDefinition& right) {
  if (left.filePath != right.filePath) {
    return left.filePath < right.filePath;
  }
  if (left.lineNumber != right.lineNumber) {
    return left.lineNumber < right.lineNumber;
  }
  return left.symbolId < right.symbolId;
}

bool symbolDefinitionSameId(const SymbolDefinition& left,
                            const SymbolDefinition& right) {
  return left.symbolId == right.symbolId;
}

}  // namespace

SymbolQueryEngine::SymbolQueryEngine(core::graph_store::GraphStore* graphStore,
                                     const std::size_t cacheCapacity)
    : graphStore_(graphStore), cache_(cacheCapacity) {}

void SymbolQueryEngine::attachGraphStore(core::graph_store::GraphStore* graphStore) {
  std::lock_guard lock(mutex_);
  graphStore_ = graphStore;
}

void SymbolQueryEngine::rebuild(const ai::RuntimeState& state,
                                const std::uint64_t stateVersion) {
  std::lock_guard lock(mutex_);
  rebuildUnlocked(state, stateVersion);
}

std::vector<SymbolDefinition> SymbolQueryEngine::findDefinition(
    const std::string& symbolName) const {
  std::lock_guard lock(mutex_);
  const std::string cacheKey = makeCacheKey("definition", symbolName);

  std::vector<SymbolDefinition> cached;
  if (cache_.get(cacheKey, version_, cached)) {
    return cached;
  }

  std::vector<SymbolDefinition> result;
  const auto it = definitionsByName_.find(symbolName);
  if (it != definitionsByName_.end()) {
    result = it->second;
  }

  cache_.put(cacheKey, version_, result);
  return result;
}

std::vector<std::string> SymbolQueryEngine::findReferences(
    const std::string& symbolName) const {
  std::lock_guard lock(mutex_);
  const std::string cacheKey = makeCacheKey("references", symbolName);

  std::vector<std::string> cached;
  if (cache_.get(cacheKey, version_, cached)) {
    return cached;
  }

  std::vector<std::string> result;
  const auto it = referencesByName_.find(symbolName);
  if (it != referencesByName_.end()) {
    result = it->second;
  }
  QueryPlanner::sortAndDedupe(result);

  cache_.put(cacheKey, version_, result);
  return result;
}

std::vector<std::string> SymbolQueryEngine::findFileDependencies(
    const std::string& filePath) const {
  std::lock_guard lock(mutex_);
  const std::string cacheKey = makeCacheKey("file_dependencies", filePath);

  std::vector<std::string> cached;
  if (cache_.get(cacheKey, version_, cached)) {
    return cached;
  }

  std::optional<std::uint32_t> fileId;
  const auto fileIdIt = fileIdByPath_.find(filePath);
  if (fileIdIt != fileIdByPath_.end()) {
    fileId = fileIdIt->second;
  } else if (graphStore_ != nullptr) {
    fileId = graphStore_->fileIdForPath(filePath);
  }

  const auto plan = planner_.planFileDependencyQuery(fileId);
  const auto traversal =
      GraphTraversal::execute(plan, fileForwardAdj_, fileReverseAdj_);

  std::vector<std::string> result;
  result.reserve(traversal.orderedNodes.size());
  for (const std::uint32_t dependencyId : traversal.orderedNodes) {
    const auto pathIt = filePathById_.find(dependencyId);
    if (pathIt == filePathById_.end()) {
      continue;
    }
    result.push_back(pathIt->second);
  }
  QueryPlanner::sortAndDedupe(result);

  cache_.put(cacheKey, version_, result);
  return result;
}

std::vector<std::string> SymbolQueryEngine::findSymbolDependencies(
    const std::string& symbolName) const {
  std::lock_guard lock(mutex_);
  const std::string cacheKey = makeCacheKey("symbol_dependencies", symbolName);

  std::vector<std::string> cached;
  if (cache_.get(cacheKey, version_, cached)) {
    return cached;
  }

  std::vector<std::uint64_t> startIds;
  const auto idsIt = symbolIdsByName_.find(symbolName);
  if (idsIt != symbolIdsByName_.end()) {
    startIds = idsIt->second;
  }

  const auto plan = planner_.planSymbolDependencyQuery(startIds);
  const auto traversal =
      GraphTraversal::execute(plan, symbolForwardAdj_, symbolReverseAdj_);

  std::vector<std::string> result;
  result.reserve(traversal.orderedNodes.size());
  for (const std::uint64_t dependencyId : traversal.orderedNodes) {
    const auto symbolIt = symbolById_.find(dependencyId);
    if (symbolIt == symbolById_.end() || symbolIt->second.name.empty()) {
      continue;
    }
    result.push_back(symbolIt->second.name);
  }
  QueryPlanner::sortAndDedupe(result);

  cache_.put(cacheKey, version_, result);
  return result;
}

std::vector<std::string> SymbolQueryEngine::findImpactRegion(
    const std::string& symbolName,
    const std::size_t maxDepth) const {
  std::lock_guard lock(mutex_);
  const std::string cacheKey = makeCacheKey("impact_region", symbolName, maxDepth);

  std::vector<std::string> cached;
  if (cache_.get(cacheKey, version_, cached)) {
    return cached;
  }

  std::vector<std::string> result;
  std::vector<std::uint32_t> startFileIds;

  const auto refsIt = referencesByName_.find(symbolName);
  if (refsIt != referencesByName_.end()) {
    result.insert(result.end(), refsIt->second.begin(), refsIt->second.end());
    for (const std::string& path : refsIt->second) {
      const auto fileIdIt = fileIdByPath_.find(path);
      if (fileIdIt == fileIdByPath_.end()) {
        continue;
      }
      startFileIds.push_back(fileIdIt->second);
    }
  }

  const auto impactPlan = planner_.planImpactRegionQuery(startFileIds, maxDepth);
  const auto impactTraversal =
      GraphTraversal::execute(impactPlan, fileForwardAdj_, fileReverseAdj_);
  for (const std::uint32_t impactedFileId : impactTraversal.orderedNodes) {
    const auto pathIt = filePathById_.find(impactedFileId);
    if (pathIt == filePathById_.end()) {
      continue;
    }
    result.push_back(pathIt->second);
  }

  const auto definitionsIt = definitionsByName_.find(symbolName);
  if (definitionsIt != definitionsByName_.end()) {
    for (const SymbolDefinition& definition : definitionsIt->second) {
      if (!definition.filePath.empty()) {
        result.push_back(definition.filePath);
      }
    }
  }

  QueryPlanner::sortAndDedupe(result);
  cache_.put(cacheKey, version_, result);
  return result;
}

std::uint64_t SymbolQueryEngine::stateVersion() const noexcept {
  std::lock_guard lock(mutex_);
  return version_;
}

bool SymbolQueryEngine::empty() const {
  std::lock_guard lock(mutex_);
  return fileIdByPath_.empty() && symbolById_.empty();
}

bool SymbolQueryEngine::isDefinitionSymbol(const ai::SymbolType symbolType) {
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

std::string SymbolQueryEngine::makeCacheKey(const std::string& op,
                                            const std::string& arg,
                                            const std::size_t depth) {
  return op + "|" + arg + "|" + std::to_string(depth);
}

void SymbolQueryEngine::rebuildUnlocked(const ai::RuntimeState& state,
                                        const std::uint64_t stateVersion) {
  fileIdByPath_.clear();
  filePathById_.clear();
  definitionsByName_.clear();
  referencesByName_.clear();
  symbolIdsByName_.clear();
  symbolById_.clear();
  fileForwardAdj_.clear();
  fileReverseAdj_.clear();
  symbolForwardAdj_.clear();
  symbolReverseAdj_.clear();

  ai::RuntimeState normalized = state;
  core::graph_store::GraphLoader::normalizeRuntimeState(normalized);
  if (normalized.symbolIndex.empty()) {
    core::graph_store::GraphLoader::rebuildSymbolIndex(normalized);
  }

  for (const ai::FileRecord& file : normalized.files) {
    const auto byPathIt = fileIdByPath_.find(file.path);
    if (byPathIt == fileIdByPath_.end() || file.fileId < byPathIt->second) {
      fileIdByPath_[file.path] = file.fileId;
    }

    const auto byIdIt = filePathById_.find(file.fileId);
    if (byIdIt == filePathById_.end() || file.path < byIdIt->second) {
      filePathById_[file.fileId] = file.path;
    }
  }

  std::map<std::string, std::set<std::string>> referenceSets;

  for (const ai::SymbolRecord& symbol : normalized.symbols) {
    if (symbol.name.empty()) {
      continue;
    }

    symbolById_[symbol.symbolId] = symbol;
    symbolIdsByName_[symbol.name].push_back(symbol.symbolId);

    if (!isDefinitionSymbol(symbol.symbolType)) {
      continue;
    }

    const auto pathIt = filePathById_.find(symbol.fileId);
    if (pathIt == filePathById_.end()) {
      continue;
    }

    SymbolDefinition definition;
    definition.symbolId = symbol.symbolId;
    definition.fileId = symbol.fileId;
    definition.filePath = pathIt->second;
    definition.lineNumber = symbol.lineNumber;
    definition.signature = symbol.signature;
    definitionsByName_[symbol.name].push_back(std::move(definition));
  }

  for (auto& [name, symbolIds] : symbolIdsByName_) {
    (void)name;
    QueryPlanner::sortAndDedupe(symbolIds);
  }

  for (auto& [name, definitions] : definitionsByName_) {
    (void)name;
    std::sort(definitions.begin(), definitions.end(), symbolDefinitionLess);
    definitions.erase(
        std::unique(definitions.begin(), definitions.end(), symbolDefinitionSameId),
        definitions.end());
  }

  for (const auto& [name, symbolNode] : normalized.symbolIndex) {
    auto& refs = referenceSets[name];
    for (const std::string& usedInPath : symbolNode.usedInFiles) {
      if (!usedInPath.empty()) {
        refs.insert(usedInPath);
      }
    }
  }

  for (const ai::SymbolDependencyEdge& edge : normalized.deps.symbolEdges) {
    const auto fromIt = symbolById_.find(edge.fromSymbolId);
    const auto toIt = symbolById_.find(edge.toSymbolId);
    if (fromIt == symbolById_.end() || toIt == symbolById_.end()) {
      continue;
    }
    if (toIt->second.name.empty()) {
      continue;
    }

    const auto fromPathIt = filePathById_.find(fromIt->second.fileId);
    if (fromPathIt == filePathById_.end()) {
      continue;
    }
    referenceSets[toIt->second.name].insert(fromPathIt->second);
  }

  for (auto& [name, refs] : referenceSets) {
    std::vector<std::string> ordered(refs.begin(), refs.end());
    QueryPlanner::sortAndDedupe(ordered);
    referencesByName_[name] = std::move(ordered);
  }

  for (const ai::FileDependencyEdge& edge : normalized.deps.fileEdges) {
    if (filePathById_.find(edge.fromFileId) == filePathById_.end() ||
        filePathById_.find(edge.toFileId) == filePathById_.end()) {
      continue;
    }
    fileForwardAdj_[edge.fromFileId].push_back(edge.toFileId);
    fileReverseAdj_[edge.toFileId].push_back(edge.fromFileId);
  }
  normalizeAdjacency(fileForwardAdj_);
  normalizeAdjacency(fileReverseAdj_);

  for (const ai::SymbolDependencyEdge& edge : normalized.deps.symbolEdges) {
    if (symbolById_.find(edge.fromSymbolId) == symbolById_.end() ||
        symbolById_.find(edge.toSymbolId) == symbolById_.end()) {
      continue;
    }
    symbolForwardAdj_[edge.fromSymbolId].push_back(edge.toSymbolId);
    symbolReverseAdj_[edge.toSymbolId].push_back(edge.fromSymbolId);
  }
  normalizeAdjacency(symbolForwardAdj_);
  normalizeAdjacency(symbolReverseAdj_);

  version_ = stateVersion;
  cache_.invalidate(version_);
  cache_.clear();
}

}  // namespace ultra::engine::query

