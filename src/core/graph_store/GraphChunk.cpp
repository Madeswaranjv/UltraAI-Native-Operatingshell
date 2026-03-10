#include "GraphChunk.h"

#include "../../ai/SymbolTable.h"

#include <algorithm>
#include <tuple>

namespace ultra::core::graph_store {

namespace {

bool fileRecordLess(const ai::FileRecord& left, const ai::FileRecord& right) {
  return std::tie(left.fileId, left.path) < std::tie(right.fileId, right.path);
}

bool fileRecordSameId(const ai::FileRecord& left, const ai::FileRecord& right) {
  return left.fileId == right.fileId;
}

bool symbolRecordLess(const ai::SymbolRecord& left, const ai::SymbolRecord& right) {
  return std::tie(left.fileId, left.lineNumber, left.name, left.signature,
                  left.symbolType, left.visibility, left.symbolId) <
         std::tie(right.fileId, right.lineNumber, right.name, right.signature,
                  right.symbolType, right.visibility, right.symbolId);
}

bool symbolRecordSameId(const ai::SymbolRecord& left,
                        const ai::SymbolRecord& right) {
  return left.symbolId == right.symbolId;
}

bool fileEdgeLess(const ai::FileDependencyEdge& left,
                  const ai::FileDependencyEdge& right) {
  return std::tie(left.fromFileId, left.toFileId) <
         std::tie(right.fromFileId, right.toFileId);
}

bool fileEdgeEqual(const ai::FileDependencyEdge& left,
                   const ai::FileDependencyEdge& right) {
  return left.fromFileId == right.fromFileId && left.toFileId == right.toFileId;
}

bool symbolEdgeLess(const ai::SymbolDependencyEdge& left,
                    const ai::SymbolDependencyEdge& right) {
  return std::tie(left.fromSymbolId, left.toSymbolId) <
         std::tie(right.fromSymbolId, right.toSymbolId);
}

bool symbolEdgeEqual(const ai::SymbolDependencyEdge& left,
                     const ai::SymbolDependencyEdge& right) {
  return left.fromSymbolId == right.fromSymbolId &&
         left.toSymbolId == right.toSymbolId;
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

}  // namespace

void GraphChunk::normalizeDeterministic() {
  std::sort(fileNodes.begin(), fileNodes.end(), fileRecordLess);
  fileNodes.erase(std::unique(fileNodes.begin(), fileNodes.end(), fileRecordSameId),
                  fileNodes.end());

  std::sort(symbolNodes.begin(), symbolNodes.end(), symbolRecordLess);
  symbolNodes.erase(
      std::unique(symbolNodes.begin(), symbolNodes.end(), symbolRecordSameId),
      symbolNodes.end());
  ai::SymbolTable::sortDeterministic(symbolNodes);

  std::sort(fileEdges.begin(), fileEdges.end(), fileEdgeLess);
  fileEdges.erase(std::unique(fileEdges.begin(), fileEdges.end(), fileEdgeEqual),
                  fileEdges.end());

  std::sort(symbolEdges.begin(), symbolEdges.end(), symbolEdgeLess);
  symbolEdges.erase(
      std::unique(symbolEdges.begin(), symbolEdges.end(), symbolEdgeEqual),
      symbolEdges.end());

  for (auto& [fileId, dependencies] : semanticSymbolDepsByFileId) {
    (void)fileId;
    std::sort(dependencies.begin(), dependencies.end(), semanticDependencyLess);
    dependencies.erase(
        std::unique(dependencies.begin(), dependencies.end(),
                    semanticDependencyEqual),
        dependencies.end());
  }
}

}  // namespace ultra::core::graph_store

