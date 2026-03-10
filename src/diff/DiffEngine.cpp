#include "DiffEngine.h"

#include "ImpactAnalyzer.h"
#include "../memory/SemanticMemory.h"
#include "RiskScorer.h"
#include "SignatureDiff.h"
#include "SymbolDiff.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <tuple>
#include <vector>

//DiffEngine.cpp
namespace ultra::diff {

namespace {

using NodeID = semantic::NodeID;
using EdgePair = std::pair<NodeID, NodeID>;

constexpr std::size_t kTraversalDepthLimit = 8U;
constexpr double kCentralitySpikeThreshold = 0.20;

struct SymbolView {
  NodeID id;
  std::string name;
  std::string definedIn;
  std::vector<std::string> usedIn;
  double centrality{0.0};
};

double clamp01(const double value) {
  if (value < 0.0) {
    return 0.0;
  }
  if (value > 1.0) {
    return 1.0;
  }
  return value;
}

double round6(const double value) {
  return std::round(value * 1000000.0) / 1000000.0;
}

std::string normalizePath(const std::string& value) {
  std::string normalized = value;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  return normalized;
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

double readNumeric(const nlohmann::json& object, const char* key) {
  if (!object.contains(key)) {
    return 0.0;
  }
  const nlohmann::json& value = object.at(key);
  if (!value.is_number()) {
    return 0.0;
  }
  return value.get<double>();
}

std::vector<std::string> jsonStringArray(const nlohmann::json& value) {
  std::vector<std::string> out;
  if (!value.is_array()) {
    return out;
  }

  out.reserve(value.size());
  for (const nlohmann::json& item : value) {
    if (!item.is_string()) {
      continue;
    }
    out.push_back(item.get<std::string>());
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool isHeaderPath(const std::string& path) {
  const std::string normalized = toLower(normalizePath(path));
  if (normalized.find("/include/") != std::string::npos) {
    return true;
  }

  const std::size_t dot = normalized.find_last_of('.');
  if (dot == std::string::npos) {
    return false;
  }

  const std::string ext = normalized.substr(dot);
  return ext == ".h" || ext == ".hh" || ext == ".hpp" || ext == ".hxx";
}

bool isPublicSymbol(const SymbolView& symbol) {
  if (symbol.definedIn.empty()) {
    return false;
  }
  return isHeaderPath(symbol.definedIn);
}

bool isFileNode(const NodeID& nodeId) {
  return nodeId.rfind("file:", 0U) == 0U;
}

NodeID makeFileNodeId(const std::string& path) {
  if (path.empty()) {
    return {};
  }
  return "file:" + path;
}

std::string moduleOf(const NodeID& fileNodeId) {
  if (!isFileNode(fileNodeId)) {
    return {};
  }

  std::string path = normalizePath(fileNodeId.substr(5U));
  if (path.empty()) {
    return {};
  }

  const std::size_t slash = path.find('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(0U, slash);
}

std::map<NodeID, SymbolView> collectSymbols(
    const ultra::memory::StateSnapshot& snapshot) {
  std::map<NodeID, SymbolView> symbols;
  for (const ultra::memory::StateNode& node : snapshot.nodes) {
    if (node.nodeType != ultra::memory::NodeType::Symbol ||
        node.nodeId.empty()) {
      continue;
    }

    SymbolView view;
    view.id = node.nodeId;
    view.name = node.data.value("name", std::string{});
    view.definedIn = node.data.value("defined_in", std::string{});
    view.usedIn = jsonStringArray(node.data.value("used_in", nlohmann::json::array()));
    view.centrality = readNumeric(node.data, "centrality");
    symbols.emplace(view.id, std::move(view));
  }
  return symbols;
}

std::set<EdgePair> collectDependencies(
    const ultra::memory::StateSnapshot& snapshot) {
  std::set<EdgePair> dependencies;
  for (const ultra::memory::StateEdge& edge : snapshot.edges) {
    if (edge.edgeType != ultra::memory::EdgeType::DependsOn ||
        edge.sourceId.empty() || edge.targetId.empty()) {
      continue;
    }
    dependencies.insert({edge.sourceId, edge.targetId});
  }
  return dependencies;
}

std::size_t computeTraversalDepth(const std::set<EdgePair>& dependencies,
                                  const std::set<NodeID>& roots) {
  if (dependencies.empty() || roots.empty()) {
    return 0U;
  }

  std::map<NodeID, std::vector<NodeID>> adjacency;
  for (const auto& [from, to] : dependencies) {
    adjacency[from].push_back(to);
  }
  for (auto& [from, targets] : adjacency) {
    (void)from;
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  }

  std::map<NodeID, std::size_t> depthByNode;
  std::queue<NodeID> queue;
  for (const NodeID& root : roots) {
    depthByNode.emplace(root, 0U);
    queue.push(root);
  }

  std::size_t maxDepth = 0U;
  while (!queue.empty()) {
    const NodeID current = queue.front();
    queue.pop();

    const std::size_t currentDepth = depthByNode.at(current);
    maxDepth = std::max(maxDepth, currentDepth);
    if (currentDepth >= kTraversalDepthLimit) {
      continue;
    }

    const auto adjIt = adjacency.find(current);
    if (adjIt == adjacency.end()) {
      continue;
    }

    for (const NodeID& next : adjIt->second) {
      if (depthByNode.find(next) != depthByNode.end()) {
        continue;
      }
      depthByNode.emplace(next, currentDepth + 1U);
      queue.push(next);
    }
  }

  return maxDepth;
}

double computeCentralityDelta(
    const std::map<NodeID, SymbolView>& oldSymbols,
    const std::map<NodeID, SymbolView>& newSymbols,
    const std::set<NodeID>& affectedSymbolIds) {
  double totalDelta = 0.0;
  std::size_t count = 0U;

  if (affectedSymbolIds.empty()) {
    for (const auto& [symbolId, oldSymbol] : oldSymbols) {
      const auto newIt = newSymbols.find(symbolId);
      if (newIt == newSymbols.end()) {
        continue;
      }
      totalDelta += std::abs(newIt->second.centrality - oldSymbol.centrality);
      ++count;
    }
    if (count == 0U) {
      return 0.0;
    }
    return clamp01(totalDelta / static_cast<double>(count));
  }

  for (const NodeID& symbolId : affectedSymbolIds) {
    const auto oldIt = oldSymbols.find(symbolId);
    const auto newIt = newSymbols.find(symbolId);
    if (oldIt == oldSymbols.end() || newIt == newSymbols.end()) {
      continue;
    }
    totalDelta += std::abs(newIt->second.centrality - oldIt->second.centrality);
    ++count;
  }

  if (count == 0U) {
    return 0.0;
  }
  return clamp01(totalDelta / static_cast<double>(count));
}

bool hasCentralitySpike(
    const std::map<NodeID, SymbolView>& oldSymbols,
    const std::map<NodeID, SymbolView>& newSymbols,
    const std::set<NodeID>& affectedSymbolIds) {
  if (affectedSymbolIds.empty()) {
    for (const auto& [symbolId, oldSymbol] : oldSymbols) {
      const auto newIt = newSymbols.find(symbolId);
      if (newIt == newSymbols.end()) {
        continue;
      }
      const double delta = std::abs(newIt->second.centrality - oldSymbol.centrality);
      if (delta > kCentralitySpikeThreshold) {
        return true;
      }
    }
    return false;
  }

  for (const NodeID& symbolId : affectedSymbolIds) {
    const auto oldIt = oldSymbols.find(symbolId);
    const auto newIt = newSymbols.find(symbolId);
    if (oldIt == oldSymbols.end() || newIt == newSymbols.end()) {
      continue;
    }
    const double delta =
        std::abs(newIt->second.centrality - oldIt->second.centrality);
    if (delta > kCentralitySpikeThreshold) {
      return true;
    }
  }
  return false;
}

bool hasPayloadModification(const SymbolView& left, const SymbolView& right) {
  return left.usedIn != right.usedIn;
}

std::string semanticNodeId(const ultra::ai::SymbolRecord& oldRecord,
                           const ultra::ai::SymbolRecord& newRecord) {
  std::uint64_t symbolId = newRecord.symbolId;
  if (symbolId == 0ULL) {
    symbolId = oldRecord.symbolId;
  }
  if (symbolId == 0ULL) {
    return {};
  }
  return "symbol:" + std::to_string(symbolId);
}

const char* semanticChangeType(const ultra::types::ChangeType changeType) {
  switch (changeType) {
    case ultra::types::ChangeType::Added:
      return "added";
    case ultra::types::ChangeType::Removed:
      return "removed";
    case ultra::types::ChangeType::Modified:
      return "modified";
    case ultra::types::ChangeType::Renamed:
      return "rename";
  }
  return "modified";
}

}  // namespace

DeltaReport DiffEngine::computeDelta(
    const std::vector<ultra::ai::SymbolRecord>& stateT1,
    const std::vector<ultra::ai::SymbolRecord>& stateT2,
    const ultra::graph::DependencyGraph& depGraph,
    ultra::memory::SemanticMemory* semanticMemory,
    const std::uint64_t semanticVersion) {
  
  DeltaReport report;

  // 1. Symbol-Level Structural Diff
  report.changeObject = SymbolDiff::compare(stateT1, stateT2);

  if (semanticMemory != nullptr) {
    for (const SymbolDelta& delta : report.changeObject) {
      const std::string nodeId = semanticNodeId(delta.oldRecord, delta.newRecord);
      if (nodeId.empty()) {
        continue;
      }
      const std::string signature =
          delta.newRecord.signature.empty() ? delta.oldRecord.signature
                                            : delta.newRecord.signature;
      semanticMemory->trackSymbolEvolution(
          nodeId, delta.symbolName, signature, semanticChangeType(delta.changeType),
          semanticVersion);
    }
  }

  // 2. Signature Diff & API Contract Break Detection
  report.contractBreaks = SignatureDiff::analyze(
      report.changeObject, depGraph, semanticMemory, semanticVersion);

  // 3. Risk Detection & Drift Scoring
  report.riskScore = RiskScorer::score(report.changeObject, depGraph);

  // 4. Refactor Impact Scoring
  report.impactMap = ImpactAnalyzer::analyze(report.changeObject, depGraph, stateT1);

  return report;
}

semantic::BranchDiffReport DiffEngine::diffBranches(
    const ultra::memory::StateSnapshot& branchA,
    const ultra::memory::StateSnapshot& branchB) {
  semantic::BranchDiffReport report;

  const std::map<NodeID, SymbolView> oldSymbols = collectSymbols(branchA);
  const std::map<NodeID, SymbolView> newSymbols = collectSymbols(branchB);
  const std::set<EdgePair> oldDependencies = collectDependencies(branchA);
  const std::set<EdgePair> newDependencies = collectDependencies(branchB);

  std::set<NodeID> allSymbolIds;
  for (const auto& [id, symbol] : oldSymbols) {
    (void)symbol;
    allSymbolIds.insert(id);
  }
  for (const auto& [id, symbol] : newSymbols) {
    (void)symbol;
    allSymbolIds.insert(id);
  }

  std::set<NodeID> affectedNodeIds;
  std::set<NodeID> affectedSymbolIds;
  std::set<NodeID> traversalRoots;
  bool publicApiRemoval = false;
  bool publicSymbolImpact = false;

  for (const NodeID& symbolId : allSymbolIds) {
    const auto oldIt = oldSymbols.find(symbolId);
    const auto newIt = newSymbols.find(symbolId);
    const bool hasOld = oldIt != oldSymbols.end();
    const bool hasNew = newIt != newSymbols.end();

    if (!hasOld && hasNew) {
      report.symbols.push_back({symbolId, semantic::DiffType::Added});
      affectedNodeIds.insert(symbolId);
      affectedSymbolIds.insert(symbolId);
      publicSymbolImpact = publicSymbolImpact || isPublicSymbol(newIt->second);
      if (!newIt->second.definedIn.empty()) {
        traversalRoots.insert(makeFileNodeId(newIt->second.definedIn));
      }
      continue;
    }

    if (hasOld && !hasNew) {
      report.symbols.push_back({symbolId, semantic::DiffType::Removed});
      affectedNodeIds.insert(symbolId);
      affectedSymbolIds.insert(symbolId);
      const bool removedPublic = isPublicSymbol(oldIt->second);
      publicApiRemoval = publicApiRemoval || removedPublic;
      publicSymbolImpact = publicSymbolImpact || removedPublic;
      if (!oldIt->second.definedIn.empty()) {
        traversalRoots.insert(makeFileNodeId(oldIt->second.definedIn));
      }
      continue;
    }

    if (!hasOld || !hasNew) {
      continue;
    }

    if (oldIt->second.name != newIt->second.name) {
      report.symbols.push_back({symbolId, semantic::DiffType::Renamed});
      report.signatures.push_back({symbolId, semantic::SignatureChange::Rename});
      affectedNodeIds.insert(symbolId);
      affectedSymbolIds.insert(symbolId);
    } else if (oldIt->second.definedIn != newIt->second.definedIn) {
      report.symbols.push_back({symbolId, semantic::DiffType::Moved});
      report.signatures.push_back(
          {symbolId, semantic::SignatureChange::Relocation});
      affectedNodeIds.insert(symbolId);
      affectedSymbolIds.insert(symbolId);
    } else if (hasPayloadModification(oldIt->second, newIt->second)) {
      report.symbols.push_back({symbolId, semantic::DiffType::Modified});
      report.signatures.push_back(
          {symbolId, semantic::SignatureChange::Signature});
      affectedNodeIds.insert(symbolId);
      affectedSymbolIds.insert(symbolId);
    }

    if (affectedSymbolIds.find(symbolId) != affectedSymbolIds.end()) {
      publicSymbolImpact =
          publicSymbolImpact || isPublicSymbol(oldIt->second) ||
          isPublicSymbol(newIt->second);
      if (!newIt->second.definedIn.empty()) {
        traversalRoots.insert(makeFileNodeId(newIt->second.definedIn));
      } else if (!oldIt->second.definedIn.empty()) {
        traversalRoots.insert(makeFileNodeId(oldIt->second.definedIn));
      }
    }
  }

  bool crossModuleDependencyIntroduction = false;
  bool internalDependencyChange = false;

  for (const auto& edge : oldDependencies) {
    if (newDependencies.find(edge) != newDependencies.end()) {
      continue;
    }
    report.dependencies.push_back(
        {edge.first, edge.second, semantic::DiffType::Removed});
    affectedNodeIds.insert(edge.first);
    affectedNodeIds.insert(edge.second);
    traversalRoots.insert(edge.first);
    traversalRoots.insert(edge.second);
    const std::string fromModule = moduleOf(edge.first);
    const std::string toModule = moduleOf(edge.second);
    if (!fromModule.empty() && !toModule.empty() && fromModule == toModule) {
      internalDependencyChange = true;
    }
  }

  for (const auto& edge : newDependencies) {
    if (oldDependencies.find(edge) != oldDependencies.end()) {
      continue;
    }
    report.dependencies.push_back(
        {edge.first, edge.second, semantic::DiffType::Added});
    affectedNodeIds.insert(edge.first);
    affectedNodeIds.insert(edge.second);
    traversalRoots.insert(edge.first);
    traversalRoots.insert(edge.second);

    const std::string fromModule = moduleOf(edge.first);
    const std::string toModule = moduleOf(edge.second);
    if (!fromModule.empty() && !toModule.empty() && fromModule != toModule) {
      crossModuleDependencyIntroduction = true;
    } else if (!fromModule.empty() && !toModule.empty()) {
      internalDependencyChange = true;
    }
  }

  std::sort(report.symbols.begin(), report.symbols.end(),
            [](const semantic::SymbolDiff& left,
               const semantic::SymbolDiff& right) {
              if (left.id != right.id) {
                return left.id < right.id;
              }
              return static_cast<int>(left.type) < static_cast<int>(right.type);
            });
  std::sort(report.signatures.begin(), report.signatures.end(),
            [](const semantic::SignatureDiff& left,
               const semantic::SignatureDiff& right) {
              if (left.id != right.id) {
                return left.id < right.id;
              }
              return static_cast<int>(left.change) <
                     static_cast<int>(right.change);
            });
  std::sort(report.dependencies.begin(), report.dependencies.end(),
            [](const semantic::DependencyDiff& left,
               const semantic::DependencyDiff& right) {
              if (left.from != right.from) {
                return left.from < right.from;
              }
              if (left.to != right.to) {
                return left.to < right.to;
              }
              return static_cast<int>(left.type) < static_cast<int>(right.type);
            });

  const bool centralitySpike =
      hasCentralitySpike(oldSymbols, newSymbols, affectedSymbolIds);

  if (publicApiRemoval || crossModuleDependencyIntroduction || centralitySpike) {
    report.overallRisk = semantic::RiskLevel::HIGH;
  } else if (!report.signatures.empty() || internalDependencyChange) {
    report.overallRisk = semantic::RiskLevel::MEDIUM;
  } else {
    report.overallRisk = semantic::RiskLevel::LOW;
  }

  const std::size_t totalNodes =
      std::max<std::size_t>(1U, std::max(branchA.nodes.size(), branchB.nodes.size()));
  const double affectedNodeCount =
      clamp01(static_cast<double>(affectedNodeIds.size()) /
              static_cast<double>(totalNodes));

  const std::size_t traversalDepth =
      computeTraversalDepth(newDependencies, traversalRoots);
  const double impactTraversalDepth =
      clamp01(static_cast<double>(traversalDepth) /
              static_cast<double>(kTraversalDepthLimit));

  const double centralityDelta =
      computeCentralityDelta(oldSymbols, newSymbols, affectedSymbolIds);

  const double publicApiWeight = publicApiRemoval ? 1.0 : (publicSymbolImpact ? 0.5 : 0.0);

  const double weightedImpact = (0.35 * affectedNodeCount) +
                                (0.25 * impactTraversalDepth) +
                                (0.25 * centralityDelta) +
                                (0.15 * publicApiWeight);
  report.impactScore = round6(clamp01(weightedImpact));

  return report;
}

nlohmann::ordered_json DiffEngine::diffBranchesJson(
    const ultra::memory::StateSnapshot& branchA,
    const ultra::memory::StateSnapshot& branchB) {
  return branchDiffToJson(diffBranches(branchA, branchB));
}

nlohmann::ordered_json DiffEngine::branchDiffToJson(
    const semantic::BranchDiffReport& report) {
  nlohmann::ordered_json payload;

  payload["symbols"] = nlohmann::ordered_json::array();
  for (const semantic::SymbolDiff& symbol : report.symbols) {
    nlohmann::ordered_json item;
    item["id"] = symbol.id;
    item["type"] = semantic::toString(symbol.type);
    payload["symbols"].push_back(std::move(item));
  }

  payload["signatures"] = nlohmann::ordered_json::array();
  for (const semantic::SignatureDiff& signature : report.signatures) {
    nlohmann::ordered_json item;
    item["id"] = signature.id;
    item["change"] = semantic::toString(signature.change);
    payload["signatures"].push_back(std::move(item));
  }

  payload["dependencies"] = nlohmann::ordered_json::array();
  for (const semantic::DependencyDiff& dependency : report.dependencies) {
    nlohmann::ordered_json item;
    item["from"] = dependency.from;
    item["to"] = dependency.to;
    item["type"] = semantic::toString(dependency.type);
    payload["dependencies"].push_back(std::move(item));
  }

  payload["risk"] = semantic::toString(report.overallRisk);
  payload["impactScore"] = round6(report.impactScore);
  return payload;
}

}  // namespace ultra::diff
