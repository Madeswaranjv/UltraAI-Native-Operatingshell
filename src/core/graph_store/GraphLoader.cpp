#include "GraphLoader.h"

#include "GraphSerializer.h"

#include "../../ai/DependencyTable.h"
#include "../../ai/SymbolTable.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace ultra::core::graph_store {

namespace {

bool readUint8(std::ifstream& input, std::uint8_t& value) {
  char ch = 0;
  if (!input.get(ch)) {
    return false;
  }
  value = static_cast<std::uint8_t>(ch);
  return true;
}

bool readUint32(std::ifstream& input, std::uint32_t& value) {
  std::array<std::uint8_t, 4> bytes{};
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  value = static_cast<std::uint32_t>(bytes[0]) |
          (static_cast<std::uint32_t>(bytes[1]) << 8U) |
          (static_cast<std::uint32_t>(bytes[2]) << 16U) |
          (static_cast<std::uint32_t>(bytes[3]) << 24U);
  return true;
}

bool readUint64(std::ifstream& input, std::uint64_t& value) {
  std::array<std::uint8_t, 8> bytes{};
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  value = 0ULL;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(i)])
             << (static_cast<std::uint64_t>(i) * 8ULL);
  }
  return true;
}

bool readString(std::ifstream& input, std::string& value) {
  std::uint32_t length = 0U;
  if (!readUint32(input, length)) {
    return false;
  }
  value.assign(length, '\0');
  if (length == 0U) {
    return true;
  }
  return static_cast<bool>(
      input.read(value.data(), static_cast<std::streamsize>(length)));
}

bool fileRecordLess(const ai::FileRecord& left, const ai::FileRecord& right) {
  if (left.path != right.path) {
    return left.path < right.path;
  }
  return left.fileId < right.fileId;
}

bool fileRecordSameId(const ai::FileRecord& left, const ai::FileRecord& right) {
  return left.fileId == right.fileId;
}

bool symbolRecordSameId(const ai::SymbolRecord& left,
                        const ai::SymbolRecord& right) {
  return left.symbolId == right.symbolId;
}

bool semanticDependencyLess(const ai::SemanticSymbolDependency& left,
                            const ai::SemanticSymbolDependency& right) {
  return std::tie(left.fromSymbol, left.toSymbol, left.lineNumber) <
         std::tie(right.fromSymbol, right.toSymbol, right.lineNumber);
}

bool semanticDependencyEqual(const ai::SemanticSymbolDependency& left,
                             const ai::SemanticSymbolDependency& right) {
  return left.fromSymbol == right.fromSymbol && left.toSymbol == right.toSymbol &&
         left.lineNumber == right.lineNumber;
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
      node.usedInFiles.insert(usageIt->second.begin(), usageIt->second.end());
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

}  // namespace

bool GraphLoader::loadManifest(const std::filesystem::path& manifestPath,
                               std::vector<ChunkManifestEntry>& entries,
                               std::string& error) const {
  entries.clear();
  std::ifstream input(manifestPath, std::ios::binary);
  if (!input) {
    error = "Failed to open graph-store manifest: " + manifestPath.string();
    return false;
  }

  std::uint32_t magic = 0U;
  std::uint32_t version = 0U;
  std::uint32_t count = 0U;
  if (!readUint32(input, magic) || !readUint32(input, version) ||
      !readUint32(input, count)) {
    error = "Failed to read graph-store manifest header.";
    return false;
  }

  if (magic != GraphSerializer::kManifestMagic) {
    error = "Graph-store manifest magic mismatch.";
    return false;
  }
  if (version != GraphSerializer::kFormatVersion) {
    error = "Graph-store manifest version mismatch.";
    return false;
  }

  entries.reserve(count);
  for (std::uint32_t index = 0U; index < count; ++index) {
    ChunkManifestEntry entry;
    if (!readUint32(input, entry.chunkId) || !readUint32(input, entry.fileCount) ||
        !readUint32(input, entry.symbolCount) ||
        !readUint32(input, entry.fileEdgeCount) ||
        !readUint32(input, entry.symbolEdgeCount) ||
        !readUint32(input, entry.semanticFileCount)) {
      error = "Failed to read graph-store manifest entry.";
      return false;
    }
    entries.push_back(entry);
  }

  std::sort(entries.begin(), entries.end(),
            [](const ChunkManifestEntry& left, const ChunkManifestEntry& right) {
              return left.chunkId < right.chunkId;
            });
  entries.erase(std::unique(entries.begin(), entries.end(),
                            [](const ChunkManifestEntry& left,
                               const ChunkManifestEntry& right) {
                              return left.chunkId == right.chunkId;
                            }),
                entries.end());
  return true;
}

bool GraphLoader::loadChunk(const std::filesystem::path& chunkPath,
                            GraphChunk& outChunk,
                            std::string& error) const {
  outChunk = GraphChunk{};
  std::ifstream input(chunkPath, std::ios::binary);
  if (!input) {
    error = "Failed to open graph-store chunk: " + chunkPath.string();
    return false;
  }

  std::uint32_t magic = 0U;
  std::uint32_t version = 0U;
  std::uint32_t chunkId = 0U;
  std::uint32_t fileCount = 0U;
  std::uint32_t symbolCount = 0U;
  std::uint32_t fileEdgeCount = 0U;
  std::uint32_t symbolEdgeCount = 0U;
  std::uint32_t semanticFileCount = 0U;

  if (!readUint32(input, magic) || !readUint32(input, version) ||
      !readUint32(input, chunkId) || !readUint32(input, fileCount) ||
      !readUint32(input, symbolCount) || !readUint32(input, fileEdgeCount) ||
      !readUint32(input, symbolEdgeCount) ||
      !readUint32(input, semanticFileCount)) {
    error = "Failed to read graph-store chunk header.";
    return false;
  }

  if (magic != GraphSerializer::kChunkMagic) {
    error = "Graph-store chunk magic mismatch.";
    return false;
  }
  if (version != GraphSerializer::kFormatVersion) {
    error = "Graph-store chunk version mismatch.";
    return false;
  }

  outChunk.chunkId = chunkId;
  outChunk.fileNodes.reserve(fileCount);
  outChunk.symbolNodes.reserve(symbolCount);
  outChunk.fileEdges.reserve(fileEdgeCount);
  outChunk.symbolEdges.reserve(symbolEdgeCount);

  for (std::uint32_t index = 0U; index < fileCount; ++index) {
    ai::FileRecord file;
    std::uint8_t language = 0U;
    if (!readUint32(input, file.fileId) || !readString(input, file.path) ||
        !input.read(reinterpret_cast<char*>(file.hash.data()),
                    static_cast<std::streamsize>(file.hash.size())) ||
        !readUint8(input, language) || !readUint64(input, file.lastModified)) {
      error = "Failed to read graph-store file record.";
      return false;
    }
    file.language = static_cast<ai::Language>(language);
    outChunk.fileNodes.push_back(std::move(file));
  }

  for (std::uint32_t index = 0U; index < symbolCount; ++index) {
    ai::SymbolRecord symbol;
    std::uint8_t symbolType = 0U;
    std::uint8_t visibility = 0U;
    if (!readUint64(input, symbol.symbolId) || !readUint32(input, symbol.fileId) ||
        !readString(input, symbol.name) || !readString(input, symbol.signature) ||
        !readUint8(input, symbolType) || !readUint8(input, visibility) ||
        !readUint32(input, symbol.lineNumber)) {
      error = "Failed to read graph-store symbol record.";
      return false;
    }
    symbol.symbolType = static_cast<ai::SymbolType>(symbolType);
    symbol.visibility = static_cast<ai::Visibility>(visibility);
    outChunk.symbolNodes.push_back(std::move(symbol));
  }

  for (std::uint32_t index = 0U; index < fileEdgeCount; ++index) {
    ai::FileDependencyEdge edge;
    if (!readUint32(input, edge.fromFileId) || !readUint32(input, edge.toFileId)) {
      error = "Failed to read graph-store file edge.";
      return false;
    }
    outChunk.fileEdges.push_back(edge);
  }

  for (std::uint32_t index = 0U; index < symbolEdgeCount; ++index) {
    ai::SymbolDependencyEdge edge;
    if (!readUint64(input, edge.fromSymbolId) ||
        !readUint64(input, edge.toSymbolId)) {
      error = "Failed to read graph-store symbol edge.";
      return false;
    }
    outChunk.symbolEdges.push_back(edge);
  }

  for (std::uint32_t index = 0U; index < semanticFileCount; ++index) {
    std::uint32_t fileId = 0U;
    std::uint32_t depCount = 0U;
    if (!readUint32(input, fileId) || !readUint32(input, depCount)) {
      error = "Failed to read graph-store semantic dependency file entry.";
      return false;
    }
    auto& deps = outChunk.semanticSymbolDepsByFileId[fileId];
    deps.reserve(depCount);
    for (std::uint32_t depIndex = 0U; depIndex < depCount; ++depIndex) {
      ai::SemanticSymbolDependency dependency;
      if (!readString(input, dependency.fromSymbol) ||
          !readString(input, dependency.toSymbol) ||
          !readUint32(input, dependency.lineNumber)) {
        error = "Failed to read graph-store semantic dependency.";
        return false;
      }
      deps.push_back(std::move(dependency));
    }
  }

  outChunk.normalizeDeterministic();
  return true;
}

bool GraphLoader::loadAll(const std::filesystem::path& chunksDir,
                          const std::filesystem::path& manifestPath,
                          ai::RuntimeState& outState,
                          std::string& error) const {
  std::vector<ChunkManifestEntry> entries;
  if (!loadManifest(manifestPath, entries, error)) {
    return false;
  }

  std::vector<std::uint32_t> chunkIds;
  chunkIds.reserve(entries.size());
  for (const ChunkManifestEntry& entry : entries) {
    chunkIds.push_back(entry.chunkId);
  }
  return loadSelected(chunksDir, chunkIds, outState, error);
}

bool GraphLoader::loadPartial(const std::filesystem::path& chunksDir,
                              const std::filesystem::path& manifestPath,
                              const std::size_t maxChunks,
                              ai::RuntimeState& outState,
                              std::string& error) const {
  std::vector<ChunkManifestEntry> entries;
  if (!loadManifest(manifestPath, entries, error)) {
    return false;
  }

  std::vector<std::uint32_t> chunkIds;
  const std::size_t bounded = std::min(maxChunks, entries.size());
  chunkIds.reserve(bounded);
  for (std::size_t index = 0U; index < bounded; ++index) {
    chunkIds.push_back(entries[index].chunkId);
  }
  return loadSelected(chunksDir, chunkIds, outState, error);
}

bool GraphLoader::loadSelected(const std::filesystem::path& chunksDir,
                               const std::vector<std::uint32_t>& chunkIds,
                               ai::RuntimeState& outState,
                               std::string& error) const {
  outState = ai::RuntimeState{};
  if (chunkIds.empty()) {
    return true;
  }

  std::vector<std::uint32_t> sortedChunkIds = chunkIds;
  std::sort(sortedChunkIds.begin(), sortedChunkIds.end());
  sortedChunkIds.erase(
      std::unique(sortedChunkIds.begin(), sortedChunkIds.end()),
      sortedChunkIds.end());

  for (const std::uint32_t chunkId : sortedChunkIds) {
    const std::filesystem::path chunkPath =
        GraphSerializer::chunkPathFor(chunksDir, chunkId);
    GraphChunk chunk;
    if (!loadChunk(chunkPath, chunk, error)) {
      return false;
    }

    outState.files.insert(outState.files.end(), chunk.fileNodes.begin(),
                          chunk.fileNodes.end());
    outState.symbols.insert(outState.symbols.end(), chunk.symbolNodes.begin(),
                            chunk.symbolNodes.end());
    outState.deps.fileEdges.insert(outState.deps.fileEdges.end(),
                                   chunk.fileEdges.begin(), chunk.fileEdges.end());
    outState.deps.symbolEdges.insert(outState.deps.symbolEdges.end(),
                                     chunk.symbolEdges.begin(),
                                     chunk.symbolEdges.end());

    for (const auto& [fileId, dependencies] :
         chunk.semanticSymbolDepsByFileId) {
      auto& target = outState.semanticSymbolDepsByFileId[fileId];
      target.insert(target.end(), dependencies.begin(), dependencies.end());
    }
  }

  normalizeRuntimeState(outState);
  rebuildSymbolIndex(outState);
  return true;
}

void GraphLoader::normalizeRuntimeState(ai::RuntimeState& state) {
  std::sort(state.files.begin(), state.files.end(), fileRecordLess);
  state.files.erase(
      std::unique(state.files.begin(), state.files.end(), fileRecordSameId),
      state.files.end());

  ai::SymbolTable::sortDeterministic(state.symbols);
  state.symbols.erase(
      std::unique(state.symbols.begin(), state.symbols.end(), symbolRecordSameId),
      state.symbols.end());

  ai::DependencyTable::sortAndDedupe(state.deps);

  for (auto& [fileId, dependencies] : state.semanticSymbolDepsByFileId) {
    (void)fileId;
    std::sort(dependencies.begin(), dependencies.end(), semanticDependencyLess);
    dependencies.erase(
        std::unique(dependencies.begin(), dependencies.end(),
                    semanticDependencyEqual),
        dependencies.end());
  }
}

void GraphLoader::rebuildSymbolIndex(ai::RuntimeState& state) {
  state.symbolIndex = buildSymbolIndex(state.files, state.symbols, state.deps);
}

}  // namespace ultra::core::graph_store

