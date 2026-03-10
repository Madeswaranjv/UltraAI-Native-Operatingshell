#pragma once

#include "../../ai/RuntimeState.h"

#include <cstdint>
#include <map>
#include <vector>

namespace ultra::core::graph_store {

struct GraphChunk {
  std::uint32_t chunkId{0U};
  std::vector<ai::FileRecord> fileNodes;
  std::vector<ai::SymbolRecord> symbolNodes;
  std::vector<ai::FileDependencyEdge> fileEdges;
  std::vector<ai::SymbolDependencyEdge> symbolEdges;
  std::map<std::uint32_t, std::vector<ai::SemanticSymbolDependency>>
      semanticSymbolDepsByFileId;

  void normalizeDeterministic();
};

struct ChunkManifestEntry {
  std::uint32_t chunkId{0U};
  std::uint32_t fileCount{0U};
  std::uint32_t symbolCount{0U};
  std::uint32_t fileEdgeCount{0U};
  std::uint32_t symbolEdgeCount{0U};
  std::uint32_t semanticFileCount{0U};
};

}  // namespace ultra::core::graph_store

