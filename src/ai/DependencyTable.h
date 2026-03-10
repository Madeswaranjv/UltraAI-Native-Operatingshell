#pragma once

#include "SymbolTable.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ultra::ai {

enum class DependencyEdgeType : std::uint8_t { File = 1, Symbol = 2 };

struct FileDependencyEdge {
  std::uint32_t fromFileId{0};
  std::uint32_t toFileId{0};
};

struct SymbolDependencyEdge {
  std::uint64_t fromSymbolId{0};
  std::uint64_t toSymbolId{0};
};

struct DependencyTableData {
  std::vector<FileDependencyEdge> fileEdges;
  std::vector<SymbolDependencyEdge> symbolEdges;
};

class DependencyTable {
 public:
  static void sortAndDedupe(DependencyTableData& table);
  static std::vector<SymbolDependencyEdge> buildSymbolEdgesFromFileEdges(
      const std::vector<FileDependencyEdge>& fileEdges,
      const std::map<std::uint32_t, std::vector<SymbolRecord>>& symbolsByFileId);
  static std::vector<SymbolDependencyEdge> buildSymbolEdgesFromSemanticDependencies(
      const std::map<std::uint32_t, std::vector<SemanticSymbolDependency>>&
          semanticDependenciesByFileId,
      const std::map<std::uint32_t, std::vector<SymbolRecord>>& symbolsByFileId);
};

}  // namespace ultra::ai
