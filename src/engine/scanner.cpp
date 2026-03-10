#include "scanner.h"
#include "../ai/DependencyTable.h"
#include "../ai/FileRegistry.h"
#include "../ai/Hashing.h"
#include "../ai/SemanticExtractor.h"
#include "../ai/SymbolTable.h"
#include "parallel/ParallelScanner.h"
#include "../runtime/CPUGovernor.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace ultra::engine {

namespace {

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
        out.push_back(normalizePathString(
            (basePath / ext.substr(1)).lexically_normal()));
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

std::map<std::string, ai::DiscoveredFile> discoveredMap(
    const std::filesystem::path& projectRoot) {
  std::map<std::string, ai::DiscoveredFile> byPath;
  const std::vector<ai::DiscoveredFile> discovered =
      ai::FileRegistry::discoverProjectFiles(projectRoot);
  for (const ai::DiscoveredFile& file : discovered) {
    byPath[file.relativePath] = file;
  }
  return byPath;
}

std::uint32_t nextFileId(const std::vector<ai::FileRecord>& files) {
  std::uint32_t maxId = 0;
  for (const ai::FileRecord& file : files) {
    if (file.fileId > maxId) {
      maxId = file.fileId;
    }
  }
  return maxId + 1U;
}

bool isDefinitionSymbol(const ai::SymbolRecord& symbol) {
  switch (symbol.symbolType) {
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

std::unordered_map<std::string, ai::SymbolNode> buildSymbolIndex(
    const std::vector<ai::FileRecord>& files,
    const std::vector<ai::SymbolRecord>& symbols,
    const ai::DependencyTableData& deps) {
  std::unordered_map<std::string, ai::SymbolNode> index;
  index.reserve(symbols.size());
  if (symbols.empty()) {
    return index;
  }

  std::unordered_map<std::uint32_t, std::string> pathByFileId;
  pathByFileId.reserve(files.size());
  for (const ai::FileRecord& file : files) {
    pathByFileId[file.fileId] = file.path;
  }

  std::unordered_map<std::uint64_t, const ai::SymbolRecord*> symbolById;
  symbolById.reserve(symbols.size());
  for (const ai::SymbolRecord& symbol : symbols) {
    symbolById[symbol.symbolId] = &symbol;
  }

  // Stage 1: collect immutable intermediate structures.
  std::unordered_map<std::string, std::vector<const ai::SymbolRecord*>>
      definitionsByName;
  definitionsByName.reserve(symbols.size());

  std::unordered_map<std::string, std::set<std::string>> usageFilesByName;
  usageFilesByName.reserve(symbols.size());

  for (const ai::SymbolRecord& symbol : symbols) {
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

  for (const ai::SymbolDependencyEdge& edge : deps.symbolEdges) {
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

  // Stage 2: build final index only from known definitions.
  for (const auto& [name, definitions] : definitionsByName) {
    if (name.empty() || definitions.empty()) {
      continue;
    }

    std::string definedIn;
    for (const ai::SymbolRecord* definition : definitions) {
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

    ai::SymbolNode node;
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

void rebuildSymbolEdges(ai::RuntimeState& state) {
  ai::SymbolTable::sortDeterministic(state.symbols);
  ai::DependencyTable::sortAndDedupe(state.deps);
  const std::map<std::uint32_t, std::vector<ai::SymbolRecord>> symbolsByFileId =
      ai::SymbolTable::groupByFileId(state.symbols);
  state.deps.symbolEdges.clear();
  const std::vector<ai::SymbolDependencyEdge> fromFileEdges =
      ai::DependencyTable::buildSymbolEdgesFromFileEdges(state.deps.fileEdges,
                                                         symbolsByFileId);
  const std::vector<ai::SymbolDependencyEdge> fromSemanticEdges =
      ai::DependencyTable::buildSymbolEdgesFromSemanticDependencies(
          state.semanticSymbolDepsByFileId, symbolsByFileId);
  state.deps.symbolEdges.insert(state.deps.symbolEdges.end(),
                                fromFileEdges.begin(), fromFileEdges.end());
  state.deps.symbolEdges.insert(state.deps.symbolEdges.end(),
                                fromSemanticEdges.begin(),
                                fromSemanticEdges.end());
  ai::DependencyTable::sortAndDedupe(state.deps);
  state.symbolIndex = buildSymbolIndex(state.files, state.symbols, state.deps);
}

void copyStateToOutput(const ai::RuntimeState& state, ScanOutput& output) {
  output.files = state.files;
  output.symbols = state.symbols;
  output.deps = state.deps;
  output.semanticSymbolDepsByFileId = state.semanticSymbolDepsByFileId;
  output.symbolIndex = state.symbolIndex;
}

}  // namespace

Scanner::Scanner(std::filesystem::path projectRoot)
    : projectRoot_(
          std::filesystem::absolute(std::move(projectRoot)).lexically_normal()) {}

std::size_t Scanner::countFiles() const {
  return ai::FileRegistry::discoverProjectFiles(projectRoot_).size();
}

bool Scanner::fullScanParallel(ScanOutput& output, std::string& error) const {
  output = ScanOutput{};
  struct ScopedWorkloadTimer {
    runtime::CPUGovernor& governor;
    const char* name;
    std::chrono::steady_clock::time_point start;

    ScopedWorkloadTimer(runtime::CPUGovernor& governorRef, const char* workloadName)
        : governor(governorRef),
          name(workloadName),
          start(std::chrono::steady_clock::now()) {
      governor.registerWorkload(name);
    }

    ~ScopedWorkloadTimer() {
      const double elapsedMs =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - start)
              .count();
      governor.recordExecutionTime(name, elapsedMs);
    }
  };
  ScopedWorkloadTimer workloadTimer(runtime::CPUGovernor::instance(),
                                    "scanner.full_scan_parallel");
  (void)workloadTimer;

  parallel::ParallelScanner parallelScanner(projectRoot_);
  parallel::ParallelScanResult parallelResult;
  const parallel::DependencyResolver dependencyResolver =
      [this](const std::string& currentFilePath,
             const std::string& reference,
             const std::map<std::string, ai::FileRecord>& currentFilesByPath,
             std::string& resolvedPath) {
        return resolveDependencyReference(currentFilePath, reference,
                                          currentFilesByPath, resolvedPath);
      };

  if (!parallelScanner.runFullScan(dependencyResolver, parallelResult, error)) {
    return false;
  }

  output.files = std::move(parallelResult.files);
  output.symbols = std::move(parallelResult.symbols);
  output.deps = std::move(parallelResult.deps);
  output.semanticSymbolDepsByFileId =
      std::move(parallelResult.semanticSymbolDepsByFileId);

  if (output.files.empty()) {
    return true;
  }

  for (const ai::FileRecord& file : output.files) {
    output.changeSet.added.insert(file.path);
  }

  output.symbolIndex = buildSymbolIndex(output.files, output.symbols, output.deps);

  for (const ai::FileRecord& file : output.files) {
    ai::ChangeLogRecord log;
    log.fileId = file.fileId;
    log.changeType = ai::ChangeType::Added;
    log.timestamp = file.lastModified;
    output.changesForLog.push_back(log);
  }

  return true;
}

bool Scanner::incrementalAdd(ai::RuntimeState& state,
                             const std::string& path,
                             ScanOutput& output,
                             std::string& error) const {
  output = ScanOutput{};

  const auto existingIt =
      std::find_if(state.files.begin(), state.files.end(),
                   [&path](const ai::FileRecord& record) {
                     return record.path == path;
                   });
  if (existingIt != state.files.end()) {
    return incrementalModify(state, path, output, error);
  }

  const std::map<std::string, ai::DiscoveredFile> discoveredByPath =
      discoveredMap(projectRoot_);
  const auto discoveredIt = discoveredByPath.find(path);
  if (discoveredIt == discoveredByPath.end()) {
    return true;
  }

  ai::FileRecord record;
  record.fileId = nextFileId(state.files);
  record.path = discoveredIt->second.relativePath;
  record.language = discoveredIt->second.language;
  record.lastModified = discoveredIt->second.lastModified;
  if (!ai::sha256OfFile(discoveredIt->second.absolutePath, record.hash, error)) {
    return false;
  }

  const ai::SemanticParseResult semantic = ai::SemanticExtractor::extract(
      discoveredIt->second.absolutePath, discoveredIt->second.language);

  state.files.push_back(record);
  std::sort(state.files.begin(), state.files.end(),
            [](const ai::FileRecord& left, const ai::FileRecord& right) {
              return left.path < right.path;
            });

  std::vector<ai::SymbolRecord> newSymbols;
  if (!ai::SymbolTable::buildFromExtracted(record.fileId, semantic.symbols,
                                           newSymbols, error)) {
    return false;
  }
  state.symbols.insert(state.symbols.end(), newSymbols.begin(), newSymbols.end());
  state.semanticSymbolDepsByFileId[record.fileId] = semantic.symbolDependencies;

  const std::map<std::string, ai::FileRecord> filesByPath =
      ai::FileRegistry::mapByPath(state.files);
  for (const std::string& reference : semantic.dependencyReferences) {
    std::string resolvedPath;
    if (!resolveDependencyReference(record.path, reference, filesByPath,
                                    resolvedPath)) {
      continue;
    }
    const auto targetIt = filesByPath.find(resolvedPath);
    if (targetIt == filesByPath.end()) {
      continue;
    }
    ai::FileDependencyEdge edge;
    edge.fromFileId = record.fileId;
    edge.toFileId = targetIt->second.fileId;
    state.deps.fileEdges.push_back(edge);
  }

  rebuildSymbolEdges(state);

  output.changeSet.added.insert(path);
  ai::ChangeLogRecord log;
  log.fileId = record.fileId;
  log.changeType = ai::ChangeType::Added;
  log.timestamp = record.lastModified;
  output.changesForLog.push_back(log);
  copyStateToOutput(state, output);
  return true;
}

bool Scanner::incrementalRemove(ai::RuntimeState& state,
                                const std::string& path,
                                ScanOutput& output,
                                std::string& error) const {
  (void)error;
  output = ScanOutput{};

  const auto fileIt =
      std::find_if(state.files.begin(), state.files.end(),
                   [&path](const ai::FileRecord& file) {
                     return file.path == path;
                   });
  if (fileIt == state.files.end()) {
    return true;
  }

  const std::uint32_t removedFileId = fileIt->fileId;
  const std::uint64_t removedTimestamp = fileIt->lastModified;
  state.files.erase(fileIt);

  state.symbols.erase(std::remove_if(state.symbols.begin(), state.symbols.end(),
                                     [removedFileId](const ai::SymbolRecord& symbol) {
                                       return symbol.fileId == removedFileId;
                                     }),
                      state.symbols.end());

  state.deps.fileEdges.erase(
      std::remove_if(state.deps.fileEdges.begin(), state.deps.fileEdges.end(),
                     [removedFileId](const ai::FileDependencyEdge& edge) {
                       return edge.fromFileId == removedFileId ||
                              edge.toFileId == removedFileId;
                     }),
      state.deps.fileEdges.end());
  state.semanticSymbolDepsByFileId.erase(removedFileId);

  rebuildSymbolEdges(state);

  output.changeSet.deleted.insert(path);
  ai::ChangeLogRecord log;
  log.fileId = removedFileId;
  log.changeType = ai::ChangeType::Deleted;
  log.timestamp = removedTimestamp;
  output.changesForLog.push_back(log);
  copyStateToOutput(state, output);
  return true;
}

bool Scanner::incrementalModify(ai::RuntimeState& state,
                                const std::string& path,
                                ScanOutput& output,
                                std::string& error) const {
  output = ScanOutput{};

  auto fileIt = std::find_if(
      state.files.begin(), state.files.end(),
      [&path](const ai::FileRecord& file) { return file.path == path; });
  if (fileIt == state.files.end()) {
    return incrementalAdd(state, path, output, error);
  }

  const std::map<std::string, ai::DiscoveredFile> discoveredByPath =
      discoveredMap(projectRoot_);
  const auto discoveredIt = discoveredByPath.find(path);
  if (discoveredIt == discoveredByPath.end()) {
    return incrementalRemove(state, path, output, error);
  }

  ai::Sha256Hash newHash{};
  if (!ai::sha256OfFile(discoveredIt->second.absolutePath, newHash, error)) {
    return false;
  }

  if (ai::hashesEqual(fileIt->hash, newHash) &&
      fileIt->lastModified == discoveredIt->second.lastModified) {
    return true;
  }

  const std::uint32_t fileId = fileIt->fileId;
  fileIt->hash = newHash;
  fileIt->lastModified = discoveredIt->second.lastModified;
  fileIt->language = discoveredIt->second.language;

  const ai::SemanticParseResult semantic = ai::SemanticExtractor::extract(
      discoveredIt->second.absolutePath, discoveredIt->second.language);

  state.symbols.erase(
      std::remove_if(state.symbols.begin(), state.symbols.end(),
                     [fileId](const ai::SymbolRecord& symbol) {
                       return symbol.fileId == fileId;
                     }),
      state.symbols.end());

  std::vector<ai::SymbolRecord> rebuiltSymbols;
  if (!ai::SymbolTable::buildFromExtracted(fileId, semantic.symbols, rebuiltSymbols,
                                           error)) {
    return false;
  }
  state.symbols.insert(state.symbols.end(), rebuiltSymbols.begin(),
                       rebuiltSymbols.end());
  state.semanticSymbolDepsByFileId[fileId] = semantic.symbolDependencies;

  state.deps.fileEdges.erase(
      std::remove_if(state.deps.fileEdges.begin(), state.deps.fileEdges.end(),
                     [fileId](const ai::FileDependencyEdge& edge) {
                       return edge.fromFileId == fileId;
                     }),
      state.deps.fileEdges.end());

  const std::map<std::string, ai::FileRecord> filesByPath =
      ai::FileRegistry::mapByPath(state.files);
  for (const std::string& reference : semantic.dependencyReferences) {
    std::string resolvedPath;
    if (!resolveDependencyReference(path, reference, filesByPath, resolvedPath)) {
      continue;
    }
    const auto targetIt = filesByPath.find(resolvedPath);
    if (targetIt == filesByPath.end()) {
      continue;
    }
    ai::FileDependencyEdge edge;
    edge.fromFileId = fileId;
    edge.toFileId = targetIt->second.fileId;
    state.deps.fileEdges.push_back(edge);
  }

  rebuildSymbolEdges(state);

  output.changeSet.modified.insert(path);
  ai::ChangeLogRecord log;
  log.fileId = fileId;
  log.changeType = ai::ChangeType::Modified;
  log.timestamp = fileIt->lastModified;
  output.changesForLog.push_back(log);
  copyStateToOutput(state, output);
  return true;
}

bool Scanner::resolveDependencyReference(
    const std::string& currentFilePath,
    const std::string& reference,
    const std::map<std::string, ai::FileRecord>& currentFilesByPath,
    std::string& resolvedPath) const {
  (void)projectRoot_;
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

}  // namespace ultra::engine
