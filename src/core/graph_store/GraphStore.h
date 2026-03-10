#pragma once

#include "GraphChunk.h"

#include "../../ai/RuntimeState.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ultra::core::graph_store {

class GraphStore {
 public:
  explicit GraphStore(std::filesystem::path baseDir,
                      std::size_t filesPerChunk = 128U);

  bool persistFull(const ai::RuntimeState& state, std::string& error);
  bool applyIncremental(const ai::RuntimeState& state,
                        const std::vector<std::uint32_t>& touchedFileIds,
                        std::string& error);

  bool load(ai::RuntimeState& outState, std::string& error);
  bool loadPartial(std::size_t maxChunks,
                   ai::RuntimeState& outState,
                   std::string& error);
  bool loadChunks(const std::vector<std::uint32_t>& chunkIds,
                  ai::RuntimeState& outState,
                  std::string& error);
  std::vector<std::uint32_t> chunkIds(std::string& error) const;

  std::optional<std::uint32_t> fileIdForPath(const std::string& path) const;
  std::optional<std::uint64_t> symbolIdForDeterministicKey(
      const ai::SymbolRecord& symbol) const;

 private:
  static std::string symbolDeterministicKey(const ai::SymbolRecord& symbol);
  static std::uint32_t chunkIdForFileId(std::uint32_t fileId,
                                        std::size_t filesPerChunk);

  bool ensureStoreLayout(std::string& error) const;
  std::map<std::uint32_t, GraphChunk> buildChunks(
      const ai::RuntimeState& state) const;
  bool writeChunks(const std::map<std::uint32_t, GraphChunk>& chunks,
                   const std::set<std::uint32_t>& chunkIdsToWrite,
                   const std::set<std::uint32_t>& chunkIdsToDelete,
                   std::string& error) const;
  void rebuildDeterministicIdMaps(const ai::RuntimeState& state);

  std::filesystem::path baseDir_;
  std::filesystem::path chunksDir_;
  std::filesystem::path manifestPath_;
  std::size_t filesPerChunk_{128U};
  std::map<std::string, std::uint32_t> fileIdByPath_;
  std::map<std::string, std::uint64_t> symbolIdByDeterministicKey_;
};

}  // namespace ultra::core::graph_store

