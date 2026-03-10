#pragma once

#include "GraphChunk.h"

#include "../../ai/RuntimeState.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::core::graph_store {

class GraphLoader {
 public:
  bool loadManifest(const std::filesystem::path& manifestPath,
                    std::vector<ChunkManifestEntry>& entries,
                    std::string& error) const;
  bool loadChunk(const std::filesystem::path& chunkPath,
                 GraphChunk& outChunk,
                 std::string& error) const;

  bool loadAll(const std::filesystem::path& chunksDir,
               const std::filesystem::path& manifestPath,
               ai::RuntimeState& outState,
               std::string& error) const;
  bool loadPartial(const std::filesystem::path& chunksDir,
                   const std::filesystem::path& manifestPath,
                   std::size_t maxChunks,
                   ai::RuntimeState& outState,
                   std::string& error) const;
  bool loadSelected(const std::filesystem::path& chunksDir,
                    const std::vector<std::uint32_t>& chunkIds,
                    ai::RuntimeState& outState,
                    std::string& error) const;

  static void normalizeRuntimeState(ai::RuntimeState& state);
  static void rebuildSymbolIndex(ai::RuntimeState& state);
};

}  // namespace ultra::core::graph_store

