#include "precision_invalidation.h"

#include "impact_analyzer.h"

#include "../ai/Hashing.h"
#include "../diff/DiffEngine.h"
#include "../graph/DependencyGraph.h"
#include "../memory/SemanticMemory.h"
#include "../types/Delta.h"

#include <external/json.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ultra::runtime {

namespace {

std::map<std::uint32_t, std::string> buildPathByFileId(
    const ai::RuntimeState& state) {
  std::map<std::uint32_t, std::string> pathByFileId;
  for (const ai::FileRecord& file : state.files) {
    pathByFileId[file.fileId] = file.path;
  }
  return pathByFileId;
}

std::set<std::pair<std::string, std::string>> dependencyEdgesByPath(
    const ai::RuntimeState& state,
    const std::map<std::uint32_t, std::string>& pathByFileId) {
  std::set<std::pair<std::string, std::string>> edges;
  for (const ai::FileDependencyEdge& edge : state.deps.fileEdges) {
    const auto fromIt = pathByFileId.find(edge.fromFileId);
    const auto toIt = pathByFileId.find(edge.toFileId);
    if (fromIt == pathByFileId.end() || toIt == pathByFileId.end()) {
      continue;
    }
    edges.insert({fromIt->second, toIt->second});
  }
  return edges;
}

std::vector<std::string> collectChangedFiles(const ai::RuntimeState& previousState,
                                             const ai::RuntimeState& currentState) {
  const auto hasHashValue = [](const ai::Sha256Hash& hash) {
    return hash != ai::zeroHash();
  };

  std::map<std::string, ai::FileRecord> oldByPath;
  std::map<std::string, ai::FileRecord> newByPath;
  for (const ai::FileRecord& file : previousState.files) {
    oldByPath[file.path] = file;
  }
  for (const ai::FileRecord& file : currentState.files) {
    newByPath[file.path] = file;
  }

  std::set<std::string> changed;
  for (const auto& [path, oldFile] : oldByPath) {
    const auto newIt = newByPath.find(path);
    if (newIt == newByPath.end()) {
      changed.insert(path);
      continue;
    }

    const bool oldHasHash = hasHashValue(oldFile.hash);
    const bool newHasHash = hasHashValue(newIt->second.hash);
    if (oldHasHash && newHasHash) {
      if (!ai::hashesEqual(oldFile.hash, newIt->second.hash)) {
        changed.insert(path);
      }
      continue;
    }

    if (!ai::hashesEqual(oldFile.hash, newIt->second.hash) ||
        oldFile.lastModified != newIt->second.lastModified) {
      changed.insert(path);
    }
  }
  for (const auto& [path, newFile] : newByPath) {
    (void)newFile;
    if (oldByPath.find(path) == oldByPath.end()) {
      changed.insert(path);
    }
  }

  return std::vector<std::string>(changed.begin(), changed.end());
}

graph::DependencyGraph buildDependencyGraph(const ai::RuntimeState& previousState,
                                            const ai::RuntimeState& currentState) {
  graph::DependencyGraph depGraph;
  std::set<std::string> nodes;
  for (const ai::FileRecord& file : previousState.files) {
    nodes.insert(file.path);
  }
  for (const ai::FileRecord& file : currentState.files) {
    nodes.insert(file.path);
  }
  for (const std::string& node : nodes) {
    depGraph.addNode(node);
  }

  const std::map<std::uint32_t, std::string> pathByFileId =
      buildPathByFileId(currentState);
  for (const ai::FileDependencyEdge& edge : currentState.deps.fileEdges) {
    const auto fromIt = pathByFileId.find(edge.fromFileId);
    const auto toIt = pathByFileId.find(edge.toFileId);
    if (fromIt == pathByFileId.end() || toIt == pathByFileId.end()) {
      continue;
    }
    depGraph.addEdge(fromIt->second, toIt->second);
  }
  return depGraph;
}

bool structurallyEqual(const ai::SymbolRecord& left, const ai::SymbolRecord& right) {
  return left.fileId == right.fileId && left.signature == right.signature &&
         left.symbolType == right.symbolType && left.visibility == right.visibility;
}

std::vector<std::pair<SymbolID, std::string>> detectRenames(
    const std::vector<diff::SymbolDelta>& deltas,
    ultra::memory::SemanticMemory* semanticMemory,
    const std::uint64_t semanticVersion) {
  std::vector<std::pair<SymbolID, std::string>> renames;

  for (const diff::SymbolDelta& delta : deltas) {
    if (delta.changeType != types::ChangeType::Renamed) {
      continue;
    }
    const SymbolID candidateId =
        delta.newRecord.symbolId != 0ULL ? delta.newRecord.symbolId
                                         : delta.oldRecord.symbolId;
    if (candidateId == 0ULL) {
      continue;
    }
    const std::string newName =
        delta.newRecord.name.empty() ? delta.symbolName : delta.newRecord.name;
    renames.push_back({candidateId, newName});
    if (semanticMemory != nullptr) {
      semanticMemory->trackSymbolEvolution(
          "symbol:" + std::to_string(candidateId), newName,
          delta.newRecord.signature, "rename", semanticVersion,
          "symbol:" + std::to_string(candidateId));
    }
  }

  std::vector<diff::SymbolDelta> removed;
  std::vector<diff::SymbolDelta> added;
  for (const diff::SymbolDelta& delta : deltas) {
    if (delta.changeType == types::ChangeType::Removed) {
      removed.push_back(delta);
    } else if (delta.changeType == types::ChangeType::Added) {
      added.push_back(delta);
    }
  }

  std::sort(removed.begin(), removed.end(),
            [](const diff::SymbolDelta& left, const diff::SymbolDelta& right) {
              if (left.oldRecord.fileId != right.oldRecord.fileId) {
                return left.oldRecord.fileId < right.oldRecord.fileId;
              }
              if (left.oldRecord.signature != right.oldRecord.signature) {
                return left.oldRecord.signature < right.oldRecord.signature;
              }
              return left.symbolName < right.symbolName;
            });
  std::sort(added.begin(), added.end(),
            [](const diff::SymbolDelta& left, const diff::SymbolDelta& right) {
              if (left.newRecord.fileId != right.newRecord.fileId) {
                return left.newRecord.fileId < right.newRecord.fileId;
              }
              if (left.newRecord.signature != right.newRecord.signature) {
                return left.newRecord.signature < right.newRecord.signature;
              }
              return left.symbolName < right.symbolName;
            });

  std::vector<bool> addedUsed(added.size(), false);
  for (const diff::SymbolDelta& oldDelta : removed) {
    for (std::size_t index = 0U; index < added.size(); ++index) {
      if (addedUsed[index]) {
        continue;
      }
      const diff::SymbolDelta& newDelta = added[index];
      if (!structurallyEqual(oldDelta.oldRecord, newDelta.newRecord)) {
        continue;
      }
      if (oldDelta.symbolName == newDelta.symbolName) {
        continue;
      }

      SymbolID symbolId = newDelta.newRecord.symbolId;
      if (symbolId == 0ULL) {
        symbolId = oldDelta.oldRecord.symbolId;
      }
      if (symbolId == 0ULL) {
        continue;
      }

      const std::string newName =
          newDelta.newRecord.name.empty() ? newDelta.symbolName
                                          : newDelta.newRecord.name;
      renames.push_back({symbolId, newName});
      if (semanticMemory != nullptr) {
        semanticMemory->trackSymbolEvolution(
            "symbol:" + std::to_string(symbolId), newName,
            newDelta.newRecord.signature, "rename", semanticVersion,
            "symbol:" + std::to_string(oldDelta.oldRecord.symbolId));
      }
      addedUsed[index] = true;
      break;
    }
  }

  std::sort(renames.begin(), renames.end(),
            [](const std::pair<SymbolID, std::string>& left,
               const std::pair<SymbolID, std::string>& right) {
              if (left.first != right.first) {
                return left.first < right.first;
              }
              return left.second < right.second;
            });
  renames.erase(std::unique(renames.begin(), renames.end()), renames.end());
  return renames;
}

bool hasSignatureDifference(const diff::DeltaReport& report) {
  for (const diff::ContractBreak& breakItem : report.contractBreaks) {
    if (breakItem.breakType != diff::BreakType::Removed) {
      return true;
    }
  }
  for (const diff::SymbolDelta& delta : report.changeObject) {
    if (delta.changeType != types::ChangeType::Modified) {
      continue;
    }
    if (delta.oldRecord.signature != delta.newRecord.signature ||
        delta.oldRecord.visibility != delta.newRecord.visibility ||
        delta.oldRecord.symbolType != delta.newRecord.symbolType) {
      return true;
    }
  }
  return false;
}

}  // namespace

DiffResult buildDiffResult(const ai::RuntimeState& previousState,
                           const ai::RuntimeState& currentState,
                           ultra::memory::SemanticMemory* semanticMemory,
                           const std::uint64_t semanticVersion) {
  DiffResult result;

  const graph::DependencyGraph depGraph =
      buildDependencyGraph(previousState, currentState);
  result.delta = diff::DiffEngine::computeDelta(previousState.symbols,
                                                currentState.symbols,
                                                depGraph,
                                                semanticMemory,
                                                semanticVersion);

  const std::map<std::uint32_t, std::string> oldPathByFileId =
      buildPathByFileId(previousState);
  const std::map<std::uint32_t, std::string> newPathByFileId =
      buildPathByFileId(currentState);

  const std::set<std::pair<std::string, std::string>> oldEdges =
      dependencyEdgesByPath(previousState, oldPathByFileId);
  const std::set<std::pair<std::string, std::string>> newEdges =
      dependencyEdgesByPath(currentState, newPathByFileId);

  for (const auto& edge : newEdges) {
    if (oldEdges.find(edge) == oldEdges.end()) {
      result.addedDependencyEdges.push_back(edge);
    }
  }
  for (const auto& edge : oldEdges) {
    if (newEdges.find(edge) == newEdges.end()) {
      result.removedDependencyEdges.push_back(edge);
    }
  }
  result.changedFiles = collectChangedFiles(previousState, currentState);

  std::set<SymbolID> affected;
  for (const diff::SymbolDelta& delta : result.delta.changeObject) {
    if (delta.oldRecord.symbolId != 0ULL) {
      affected.insert(delta.oldRecord.symbolId);
    }
    if (delta.newRecord.symbolId != 0ULL) {
      affected.insert(delta.newRecord.symbolId);
    }
  }
  if (!result.changedFiles.empty()) {
    const std::set<std::string> changedPaths(result.changedFiles.begin(),
                                             result.changedFiles.end());
    const std::map<std::uint32_t, std::string> currentPathByFileId =
        buildPathByFileId(currentState);
    for (const ai::SymbolRecord& symbol : currentState.symbols) {
      const auto pathIt = currentPathByFileId.find(symbol.fileId);
      if (pathIt == currentPathByFileId.end()) {
        continue;
      }
      if (changedPaths.find(pathIt->second) != changedPaths.end() &&
          symbol.symbolId != 0ULL) {
        affected.insert(symbol.symbolId);
      }
    }
  }
  result.affectedSymbols.assign(affected.begin(), affected.end());
  result.renamedSymbols =
      detectRenames(result.delta.changeObject, semanticMemory, semanticVersion);
  return result;
}

StructuralChangeType classifyChange(const DiffResult& diff) {
  if (!diff.renamedSymbols.empty()) {
    return StructuralChangeType::SYMBOL_RENAME;
  }

  bool publicRemoval = false;
  for (const diff::SymbolDelta& delta : diff.delta.changeObject) {
    if (delta.changeType != types::ChangeType::Removed) {
      continue;
    }
    if (delta.oldRecord.visibility == ai::Visibility::Public) {
      publicRemoval = true;
      break;
    }
  }
  if (publicRemoval) {
    return StructuralChangeType::API_REMOVAL;
  }

  if (hasSignatureDifference(diff.delta)) {
    return StructuralChangeType::SIGNATURE_CHANGE;
  }

  if (!diff.addedDependencyEdges.empty() || !diff.removedDependencyEdges.empty()) {
    return StructuralChangeType::DEPENDENCY_CHANGE;
  }

  if (!diff.delta.changeObject.empty()) {
    return StructuralChangeType::SIGNATURE_CHANGE;
  }

  return StructuralChangeType::BODY_CHANGE;
}

std::vector<SymbolID> computeImpactDepthLimited(const GraphSnapshot& snapshot,
                                                const SymbolID root,
                                                const std::size_t depthLimit) {
  std::set<SymbolID> impactedIds;
  impactedIds.insert(root);
  if (!snapshot.graph || depthLimit == 0U) {
    return std::vector<SymbolID>(impactedIds.begin(), impactedIds.end());
  }

  std::map<SymbolID, std::string> nameById;
  std::map<SymbolID, std::string> definedInById;
  std::map<SymbolID, std::set<std::string>> usedInById;

  for (const memory::StateNode& node :
       snapshot.graph->queryByType(memory::NodeType::Symbol)) {
    const SymbolID id = node.data.value("symbol_id", 0ULL);
    const std::string name = node.data.value("name", std::string{});
    if (id == 0ULL || name.empty()) {
      continue;
    }
    nameById[id] = name;
    definedInById[id] = node.data.value("defined_in", std::string{});
    if (node.data.contains("used_in") && node.data["used_in"].is_array()) {
      for (const auto& item : node.data["used_in"]) {
        if (item.is_string()) {
          usedInById[id].insert(item.get<std::string>());
        }
      }
    }
  }

  const auto rootIt = nameById.find(root);
  if (rootIt == nameById.end()) {
    return std::vector<SymbolID>(impactedIds.begin(), impactedIds.end());
  }

  ImpactAnalyzer analyzer(snapshot);
  const nlohmann::json impact = analyzer.analyzeSymbolImpact(rootIt->second, depthLimit);

  std::set<std::string> impactedFiles;
  const auto collectFiles = [&impactedFiles](const nlohmann::json& array) {
    if (!array.is_array()) {
      return;
    }
    for (const auto& item : array) {
      if (item.is_string()) {
        impactedFiles.insert(item.get<std::string>());
      }
    }
  };

  impactedFiles.insert(impact.value("defined_in", std::string{}));
  collectFiles(impact.value("direct_usage_files", nlohmann::json::array()));
  collectFiles(impact.value("transitive_impacted_files", nlohmann::json::array()));

  for (const auto& [id, definedPath] : definedInById) {
    if (!definedPath.empty() && impactedFiles.find(definedPath) != impactedFiles.end()) {
      impactedIds.insert(id);
      continue;
    }
    const auto usedIt = usedInById.find(id);
    if (usedIt == usedInById.end()) {
      continue;
    }
    for (const std::string& path : usedIt->second) {
      if (impactedFiles.find(path) != impactedFiles.end()) {
        impactedIds.insert(id);
        break;
      }
    }
  }

  return std::vector<SymbolID>(impactedIds.begin(), impactedIds.end());
}

}  // namespace ultra::runtime
