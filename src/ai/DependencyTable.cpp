#include "DependencyTable.h"

#include <algorithm>
#include <tuple>
#include <unordered_map>

namespace ultra::ai {

namespace {

bool fileEdgeLess(const FileDependencyEdge& left, const FileDependencyEdge& right) {
  return std::tie(left.fromFileId, left.toFileId) <
         std::tie(right.fromFileId, right.toFileId);
}

bool fileEdgeEqual(const FileDependencyEdge& left, const FileDependencyEdge& right) {
  return left.fromFileId == right.fromFileId && left.toFileId == right.toFileId;
}

bool symbolEdgeLess(const SymbolDependencyEdge& left,
                    const SymbolDependencyEdge& right) {
  return std::tie(left.fromSymbolId, left.toSymbolId) <
         std::tie(right.fromSymbolId, right.toSymbolId);
}

bool symbolEdgeEqual(const SymbolDependencyEdge& left,
                     const SymbolDependencyEdge& right) {
  return left.fromSymbolId == right.fromSymbolId &&
         left.toSymbolId == right.toSymbolId;
}

std::uint64_t resolveSymbolIdInFile(
    const std::vector<SymbolRecord>& symbols,
    const std::string& name) {
  std::uint64_t bestId = 0U;
  bool preferredFound = false;
  for (const SymbolRecord& symbol : symbols) {
    if (symbol.name != name) {
      continue;
    }
    const bool isPreferred = symbol.symbolType != SymbolType::Import;
    if (bestId == 0U ||
        (isPreferred && !preferredFound) ||
        (isPreferred == preferredFound && symbol.symbolId < bestId)) {
      bestId = symbol.symbolId;
      preferredFound = isPreferred;
    }
  }
  return bestId;
}

std::uint64_t resolveGlobalSymbolId(
    const std::unordered_map<std::string, std::vector<SymbolRecord>>& symbolsByName,
    const std::string& name) {
  const auto it = symbolsByName.find(name);
  if (it == symbolsByName.end()) {
    return 0U;
  }

  std::uint64_t bestId = 0U;
  bool preferredFound = false;
  for (const SymbolRecord& symbol : it->second) {
    const bool isPreferred = symbol.symbolType != SymbolType::Import;
    if (bestId == 0U ||
        (isPreferred && !preferredFound) ||
        (isPreferred == preferredFound && symbol.symbolId < bestId)) {
      bestId = symbol.symbolId;
      preferredFound = isPreferred;
    }
  }
  return bestId;
}

}  // namespace

void DependencyTable::sortAndDedupe(DependencyTableData& table) {
  std::sort(table.fileEdges.begin(), table.fileEdges.end(), fileEdgeLess);
  table.fileEdges.erase(
      std::unique(table.fileEdges.begin(), table.fileEdges.end(), fileEdgeEqual),
      table.fileEdges.end());

  std::sort(table.symbolEdges.begin(), table.symbolEdges.end(), symbolEdgeLess);
  table.symbolEdges.erase(
      std::unique(table.symbolEdges.begin(), table.symbolEdges.end(),
                  symbolEdgeEqual),
      table.symbolEdges.end());
}

std::vector<SymbolDependencyEdge> DependencyTable::buildSymbolEdgesFromFileEdges(
    const std::vector<FileDependencyEdge>& fileEdges,
    const std::map<std::uint32_t, std::vector<SymbolRecord>>& symbolsByFileId) {
  std::vector<SymbolDependencyEdge> symbolEdges;
  symbolEdges.reserve(fileEdges.size());

  for (const FileDependencyEdge& fileEdge : fileEdges) {
    const auto fromIt = symbolsByFileId.find(fileEdge.fromFileId);
    const auto toIt = symbolsByFileId.find(fileEdge.toFileId);
    if (fromIt == symbolsByFileId.end() || toIt == symbolsByFileId.end()) {
      continue;
    }
    if (fromIt->second.empty() || toIt->second.empty()) {
      continue;
    }

    SymbolDependencyEdge symbolEdge;
    symbolEdge.fromSymbolId = fromIt->second.front().symbolId;
    symbolEdge.toSymbolId = toIt->second.front().symbolId;
    symbolEdges.push_back(symbolEdge);
  }

  std::sort(symbolEdges.begin(), symbolEdges.end(), symbolEdgeLess);
  symbolEdges.erase(
      std::unique(symbolEdges.begin(), symbolEdges.end(), symbolEdgeEqual),
      symbolEdges.end());
  return symbolEdges;
}

std::vector<SymbolDependencyEdge>
DependencyTable::buildSymbolEdgesFromSemanticDependencies(
    const std::map<std::uint32_t, std::vector<SemanticSymbolDependency>>&
        semanticDependenciesByFileId,
    const std::map<std::uint32_t, std::vector<SymbolRecord>>& symbolsByFileId) {
  std::vector<SymbolDependencyEdge> symbolEdges;
  if (semanticDependenciesByFileId.empty() || symbolsByFileId.empty()) {
    return symbolEdges;
  }

  std::unordered_map<std::string, std::vector<SymbolRecord>> symbolsByName;
  for (const auto& [fileId, symbols] : symbolsByFileId) {
    (void)fileId;
    for (const SymbolRecord& symbol : symbols) {
      symbolsByName[symbol.name].push_back(symbol);
    }
  }
  for (auto& [name, symbols] : symbolsByName) {
    (void)name;
    std::sort(symbols.begin(), symbols.end(),
              [](const SymbolRecord& left, const SymbolRecord& right) {
                return left.symbolId < right.symbolId;
              });
  }

  for (const auto& [fileId, candidates] : semanticDependenciesByFileId) {
    const auto fromSymbolsIt = symbolsByFileId.find(fileId);
    if (fromSymbolsIt == symbolsByFileId.end()) {
      continue;
    }

    for (const SemanticSymbolDependency& candidate : candidates) {
      if (candidate.fromSymbol.empty() || candidate.toSymbol.empty()) {
        continue;
      }

      const std::uint64_t fromId =
          resolveSymbolIdInFile(fromSymbolsIt->second, candidate.fromSymbol);
      const std::uint64_t toId =
          resolveGlobalSymbolId(symbolsByName, candidate.toSymbol);
      if (fromId == 0U || toId == 0U || fromId == toId) {
        continue;
      }

      SymbolDependencyEdge edge;
      edge.fromSymbolId = fromId;
      edge.toSymbolId = toId;
      symbolEdges.push_back(edge);
    }
  }

  std::sort(symbolEdges.begin(), symbolEdges.end(), symbolEdgeLess);
  symbolEdges.erase(
      std::unique(symbolEdges.begin(), symbolEdges.end(), symbolEdgeEqual),
      symbolEdges.end());
  return symbolEdges;
}

}  // namespace ultra::ai
