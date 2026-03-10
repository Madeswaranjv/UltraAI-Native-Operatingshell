#pragma once

#include "../../ai/RuntimeState.h"
#include "GraphTraversal.h"
#include "QueryCache.h"
#include "QueryPlanner.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ultra::core::graph_store {
class GraphStore;
}

namespace ultra::engine::query {

struct SymbolDefinition {
  std::uint64_t symbolId{0U};
  std::uint32_t fileId{0U};
  std::string filePath;
  std::uint32_t lineNumber{0U};
  std::string signature;

  bool operator==(const SymbolDefinition& other) const noexcept {
    return symbolId == other.symbolId && fileId == other.fileId &&
           filePath == other.filePath && lineNumber == other.lineNumber &&
           signature == other.signature;
  }
};

class SymbolQueryEngine {
 public:
  explicit SymbolQueryEngine(core::graph_store::GraphStore* graphStore = nullptr,
                             std::size_t cacheCapacity = 256U);

  void attachGraphStore(core::graph_store::GraphStore* graphStore);
  void rebuild(const ai::RuntimeState& state, std::uint64_t stateVersion);

  [[nodiscard]] std::vector<SymbolDefinition> findDefinition(
      const std::string& symbolName) const;
  [[nodiscard]] std::vector<std::string> findReferences(
      const std::string& symbolName) const;
  [[nodiscard]] std::vector<std::string> findFileDependencies(
      const std::string& filePath) const;
  [[nodiscard]] std::vector<std::string> findSymbolDependencies(
      const std::string& symbolName) const;
  [[nodiscard]] std::vector<std::string> findImpactRegion(
      const std::string& symbolName,
      std::size_t maxDepth = 2U) const;

  [[nodiscard]] std::uint64_t stateVersion() const noexcept;
  [[nodiscard]] bool empty() const;

 private:
  static bool isDefinitionSymbol(ai::SymbolType symbolType);
  static std::string makeCacheKey(const std::string& op,
                                  const std::string& arg,
                                  std::size_t depth = 0U);

  template <typename NodeId>
  static void normalizeAdjacency(
      std::map<NodeId, std::vector<NodeId>>& adjacency) {
    for (auto& [nodeId, neighbors] : adjacency) {
      (void)nodeId;
      QueryPlanner::sortAndDedupe(neighbors);
    }
  }

  void rebuildUnlocked(const ai::RuntimeState& state, std::uint64_t stateVersion);

  mutable std::mutex mutex_;

  core::graph_store::GraphStore* graphStore_{nullptr};
  QueryPlanner planner_;
  mutable QueryCache cache_;

  std::uint64_t version_{0U};
  std::map<std::string, std::uint32_t> fileIdByPath_;
  std::map<std::uint32_t, std::string> filePathById_;
  std::map<std::string, std::vector<SymbolDefinition>> definitionsByName_;
  std::map<std::string, std::vector<std::string>> referencesByName_;
  std::map<std::string, std::vector<std::uint64_t>> symbolIdsByName_;
  std::map<std::uint64_t, ai::SymbolRecord> symbolById_;
  std::map<std::uint32_t, std::vector<std::uint32_t>> fileForwardAdj_;
  std::map<std::uint32_t, std::vector<std::uint32_t>> fileReverseAdj_;
  std::map<std::uint64_t, std::vector<std::uint64_t>> symbolForwardAdj_;
  std::map<std::uint64_t, std::vector<std::uint64_t>> symbolReverseAdj_;
};

}  // namespace ultra::engine::query

