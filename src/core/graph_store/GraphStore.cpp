#include "GraphStore.h"

#include "GraphLoader.h"
#include "GraphSerializer.h"

#include "../../ai/SymbolTable.h"

#include <algorithm>
#include <sstream>

namespace ultra::core::graph_store {

GraphStore::GraphStore(std::filesystem::path baseDir, const std::size_t filesPerChunk)
    : baseDir_(std::filesystem::absolute(std::move(baseDir)).lexically_normal()),
      chunksDir_(baseDir_ / "chunks"),
      manifestPath_(baseDir_ / "manifest.gman"),
      filesPerChunk_(std::max<std::size_t>(1U, filesPerChunk)) {}

bool GraphStore::persistFull(const ai::RuntimeState& state, std::string& error) {
  if (!ensureStoreLayout(error)) {
    return false;
  }

  const std::map<std::uint32_t, GraphChunk> chunks = buildChunks(state);
  std::set<std::uint32_t> toWrite;
  for (const auto& [chunkId, chunk] : chunks) {
    (void)chunk;
    toWrite.insert(chunkId);
  }

  std::set<std::uint32_t> toDelete;
  for (const std::uint32_t existing : chunkIds(error)) {
    if (chunks.find(existing) == chunks.end()) {
      toDelete.insert(existing);
    }
  }
  error.clear();

  if (!writeChunks(chunks, toWrite, toDelete, error)) {
    return false;
  }

  rebuildDeterministicIdMaps(state);
  return true;
}

bool GraphStore::applyIncremental(const ai::RuntimeState& state,
                                  const std::vector<std::uint32_t>& touchedFileIds,
                                  std::string& error) {
  if (!ensureStoreLayout(error)) {
    return false;
  }

  const std::map<std::uint32_t, GraphChunk> chunks = buildChunks(state);

  std::set<std::uint32_t> existingChunkIds;
  for (const std::uint32_t id : chunkIds(error)) {
    existingChunkIds.insert(id);
  }
  error.clear();

  std::set<std::uint32_t> touchedChunkIds;
  for (const std::uint32_t fileId : touchedFileIds) {
    const std::uint32_t chunkId = chunkIdForFileId(fileId, filesPerChunk_);
    if (chunkId != 0U) {
      touchedChunkIds.insert(chunkId);
    }
  }

  std::set<std::uint32_t> toWrite;
  if (touchedChunkIds.empty()) {
    for (const auto& [chunkId, chunk] : chunks) {
      (void)chunk;
      toWrite.insert(chunkId);
    }
  } else {
    for (const std::uint32_t chunkId : touchedChunkIds) {
      if (chunks.find(chunkId) != chunks.end()) {
        toWrite.insert(chunkId);
      }
    }
    for (const auto& [chunkId, chunk] : chunks) {
      (void)chunk;
      if (existingChunkIds.find(chunkId) == existingChunkIds.end()) {
        toWrite.insert(chunkId);
      }
    }
  }

  std::set<std::uint32_t> toDelete;
  for (const std::uint32_t existingId : existingChunkIds) {
    if (chunks.find(existingId) == chunks.end()) {
      toDelete.insert(existingId);
    }
  }

  if (!writeChunks(chunks, toWrite, toDelete, error)) {
    return false;
  }

  rebuildDeterministicIdMaps(state);
  return true;
}

bool GraphStore::load(ai::RuntimeState& outState, std::string& error) {
  GraphLoader loader;
  if (!loader.loadAll(chunksDir_, manifestPath_, outState, error)) {
    return false;
  }
  rebuildDeterministicIdMaps(outState);
  return true;
}

bool GraphStore::loadPartial(const std::size_t maxChunks,
                             ai::RuntimeState& outState,
                             std::string& error) {
  if (maxChunks == 0U) {
    return load(outState, error);
  }

  GraphLoader loader;
  if (!loader.loadPartial(chunksDir_, manifestPath_, maxChunks, outState, error)) {
    return false;
  }
  rebuildDeterministicIdMaps(outState);
  return true;
}

bool GraphStore::loadChunks(const std::vector<std::uint32_t>& chunkIdsToLoad,
                            ai::RuntimeState& outState,
                            std::string& error) {
  GraphLoader loader;
  if (!loader.loadSelected(chunksDir_, chunkIdsToLoad, outState, error)) {
    return false;
  }
  rebuildDeterministicIdMaps(outState);
  return true;
}

std::vector<std::uint32_t> GraphStore::chunkIds(std::string& error) const {
  std::vector<std::uint32_t> ids;
  if (!std::filesystem::exists(manifestPath_)) {
    error.clear();
    return ids;
  }

  GraphLoader loader;
  std::vector<ChunkManifestEntry> entries;
  if (!loader.loadManifest(manifestPath_, entries, error)) {
    return ids;
  }

  ids.reserve(entries.size());
  for (const ChunkManifestEntry& entry : entries) {
    ids.push_back(entry.chunkId);
  }
  return ids;
}

std::optional<std::uint32_t> GraphStore::fileIdForPath(
    const std::string& path) const {
  const auto it = fileIdByPath_.find(path);
  if (it == fileIdByPath_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<std::uint64_t> GraphStore::symbolIdForDeterministicKey(
    const ai::SymbolRecord& symbol) const {
  const std::string key = symbolDeterministicKey(symbol);
  const auto it = symbolIdByDeterministicKey_.find(key);
  if (it == symbolIdByDeterministicKey_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string GraphStore::symbolDeterministicKey(const ai::SymbolRecord& symbol) {
  std::ostringstream stream;
  stream << symbol.fileId << '|' << symbol.lineNumber << '|'
         << static_cast<int>(symbol.symbolType) << '|'
         << static_cast<int>(symbol.visibility) << '|' << symbol.name << '|'
         << symbol.signature;
  return stream.str();
}

std::uint32_t GraphStore::chunkIdForFileId(const std::uint32_t fileId,
                                           const std::size_t filesPerChunk) {
  if (fileId == 0U || filesPerChunk == 0U) {
    return 0U;
  }
  return static_cast<std::uint32_t>((static_cast<std::size_t>(fileId) - 1U) /
                                    filesPerChunk);
}

bool GraphStore::ensureStoreLayout(std::string& error) const {
  try {
    std::filesystem::create_directories(chunksDir_);
  } catch (const std::exception& ex) {
    error = std::string("Failed to create graph-store directories: ") + ex.what();
    return false;
  }
  return true;
}

std::map<std::uint32_t, GraphChunk> GraphStore::buildChunks(
    const ai::RuntimeState& state) const {
  std::map<std::uint32_t, GraphChunk> chunks;
  std::map<std::uint32_t, std::uint32_t> chunkByFileId;

  for (const ai::FileRecord& file : state.files) {
    const std::uint32_t chunkId = chunkIdForFileId(file.fileId, filesPerChunk_);
    if (chunkId == 0U && file.fileId == 0U) {
      continue;
    }
    GraphChunk& chunk = chunks[chunkId];
    chunk.chunkId = chunkId;
    chunk.fileNodes.push_back(file);
    chunkByFileId[file.fileId] = chunkId;
  }

  for (const ai::SymbolRecord& symbol : state.symbols) {
    std::uint32_t chunkId = 0U;
    const auto byFileIt = chunkByFileId.find(symbol.fileId);
    if (byFileIt != chunkByFileId.end()) {
      chunkId = byFileIt->second;
    } else {
      const std::uint32_t extractedFileId =
          ai::SymbolTable::extractFileId(symbol.symbolId);
      chunkId = chunkIdForFileId(extractedFileId, filesPerChunk_);
    }
    if (chunkId == 0U && symbol.fileId == 0U) {
      continue;
    }
    GraphChunk& chunk = chunks[chunkId];
    chunk.chunkId = chunkId;
    chunk.symbolNodes.push_back(symbol);
  }

  for (const ai::FileDependencyEdge& edge : state.deps.fileEdges) {
    const std::uint32_t chunkId =
        chunkIdForFileId(edge.fromFileId, filesPerChunk_);
    if (chunkId == 0U && edge.fromFileId == 0U) {
      continue;
    }
    GraphChunk& chunk = chunks[chunkId];
    chunk.chunkId = chunkId;
    chunk.fileEdges.push_back(edge);
  }

  for (const ai::SymbolDependencyEdge& edge : state.deps.symbolEdges) {
    const std::uint32_t fromFileId = ai::SymbolTable::extractFileId(edge.fromSymbolId);
    const std::uint32_t chunkId = chunkIdForFileId(fromFileId, filesPerChunk_);
    if (chunkId == 0U && fromFileId == 0U) {
      continue;
    }
    GraphChunk& chunk = chunks[chunkId];
    chunk.chunkId = chunkId;
    chunk.symbolEdges.push_back(edge);
  }

  for (const auto& [fileId, dependencies] : state.semanticSymbolDepsByFileId) {
    const std::uint32_t chunkId = chunkIdForFileId(fileId, filesPerChunk_);
    if (chunkId == 0U && fileId == 0U) {
      continue;
    }
    GraphChunk& chunk = chunks[chunkId];
    chunk.chunkId = chunkId;
    auto& target = chunk.semanticSymbolDepsByFileId[fileId];
    target.insert(target.end(), dependencies.begin(), dependencies.end());
  }

  for (auto& [chunkId, chunk] : chunks) {
    (void)chunkId;
    chunk.normalizeDeterministic();
  }

  return chunks;
}

bool GraphStore::writeChunks(const std::map<std::uint32_t, GraphChunk>& chunks,
                             const std::set<std::uint32_t>& chunkIdsToWrite,
                             const std::set<std::uint32_t>& chunkIdsToDelete,
                             std::string& error) const {
  for (const std::uint32_t chunkId : chunkIdsToWrite) {
    const auto it = chunks.find(chunkId);
    if (it == chunks.end()) {
      continue;
    }
    if (!GraphSerializer::writeChunk(chunksDir_, it->second, error)) {
      return false;
    }
  }

  for (const std::uint32_t chunkId : chunkIdsToDelete) {
    const std::filesystem::path path =
        GraphSerializer::chunkPathFor(chunksDir_, chunkId);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (ec) {
      error = "Failed to delete stale graph-store chunk: " + path.string();
      return false;
    }
  }

  std::vector<ChunkManifestEntry> manifestEntries;
  manifestEntries.reserve(chunks.size());
  for (const auto& [chunkId, chunk] : chunks) {
    ChunkManifestEntry entry;
    entry.chunkId = chunkId;
    entry.fileCount = static_cast<std::uint32_t>(chunk.fileNodes.size());
    entry.symbolCount = static_cast<std::uint32_t>(chunk.symbolNodes.size());
    entry.fileEdgeCount = static_cast<std::uint32_t>(chunk.fileEdges.size());
    entry.symbolEdgeCount = static_cast<std::uint32_t>(chunk.symbolEdges.size());
    entry.semanticFileCount =
        static_cast<std::uint32_t>(chunk.semanticSymbolDepsByFileId.size());
    manifestEntries.push_back(entry);
  }

  return GraphSerializer::writeManifest(manifestPath_, manifestEntries, error);
}

void GraphStore::rebuildDeterministicIdMaps(const ai::RuntimeState& state) {
  fileIdByPath_.clear();
  symbolIdByDeterministicKey_.clear();

  std::vector<ai::FileRecord> files = state.files;
  std::sort(files.begin(), files.end(),
            [](const ai::FileRecord& left, const ai::FileRecord& right) {
              if (left.path != right.path) {
                return left.path < right.path;
              }
              return left.fileId < right.fileId;
            });
  for (const ai::FileRecord& file : files) {
    fileIdByPath_[file.path] = file.fileId;
  }

  std::vector<ai::SymbolRecord> symbols = state.symbols;
  std::sort(symbols.begin(), symbols.end(),
            [](const ai::SymbolRecord& left, const ai::SymbolRecord& right) {
              if (left.fileId != right.fileId) {
                return left.fileId < right.fileId;
              }
              if (left.lineNumber != right.lineNumber) {
                return left.lineNumber < right.lineNumber;
              }
              if (left.name != right.name) {
                return left.name < right.name;
              }
              if (left.signature != right.signature) {
                return left.signature < right.signature;
              }
              return left.symbolId < right.symbolId;
            });
  for (const ai::SymbolRecord& symbol : symbols) {
    const std::string key = symbolDeterministicKey(symbol);
    const auto it = symbolIdByDeterministicKey_.find(key);
    if (it == symbolIdByDeterministicKey_.end() || symbol.symbolId < it->second) {
      symbolIdByDeterministicKey_[key] = symbol.symbolId;
    }
  }
}

}  // namespace ultra::core::graph_store

