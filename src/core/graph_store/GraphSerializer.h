#pragma once

#include "GraphChunk.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::core::graph_store {

class GraphSerializer {
 public:
  static constexpr std::uint32_t kChunkMagic = 0x4B484347U;     // "GCHK"
  static constexpr std::uint32_t kManifestMagic = 0x4E414D47U;  // "GMAN"
  static constexpr std::uint32_t kFormatVersion = 1U;

  static std::filesystem::path chunkPathFor(const std::filesystem::path& chunksDir,
                                            std::uint32_t chunkId);
  static bool writeChunk(const std::filesystem::path& chunksDir,
                         const GraphChunk& chunk,
                         std::string& error);
  static bool writeManifest(const std::filesystem::path& manifestPath,
                            const std::vector<ChunkManifestEntry>& entries,
                            std::string& error);
};

}  // namespace ultra::core::graph_store

