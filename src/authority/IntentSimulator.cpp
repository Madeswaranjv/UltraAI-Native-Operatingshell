#include "IntentSimulator.h"

#include "UltraAuthorityAPI.h"
//E:\Projects\Ultra\src\authority\IntentSimulator.cpp
#include "../core/state_manager.h"
#include "../diff/DiffEngine.h"
#include "../engine/impact/ImpactPredictionEngine.h"
#include "../memory/StateEdge.h"
#include "../memory/StateNode.h"
#include "../memory/StateSnapshot.h"
#include "../runtime/cognitive/CognitiveRuntime.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ultra::authority {

namespace {

std::string lowerCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool containsKeyword(const std::string& lowerText,
                     const std::initializer_list<const char*> keywords) {
  for (const char* keyword : keywords) {
    if (keyword != nullptr && lowerText.find(keyword) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool isRemovalIntent(const std::string& goal) {
  const std::string lowerGoal = lowerCopy(goal);
  return containsKeyword(lowerGoal, {"remove", "delete", "drop", "deprecate"});
}

bool isSignatureIntent(const std::string& goal) {
  const std::string lowerGoal = lowerCopy(goal);
  return containsKeyword(lowerGoal,
                         {"signature", "api", "interface", "contract", "rename"});
}

bool isDependencyIntent(const std::string& goal) {
  const std::string lowerGoal = lowerCopy(goal);
  return containsKeyword(lowerGoal, {"dependency", "import", "include", "module"});
}

std::string normalizePath(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  std::string normalized = value;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  if (normalized.size() >= 2U && normalized[0] == '.' && normalized[1] == '/') {
    normalized.erase(0, 2U);
  }
  return normalized;
}

std::string symbolNodeId(const std::uint64_t symbolId) {
  return "symbol:" + std::to_string(symbolId);
}

std::string fileNodeId(const std::string& path) {
  return "file:" + normalizePath(path);
}

std::size_t computeImpactDepth(const engine::impact::ImpactPrediction& prediction) {
  std::size_t depth = 0U;
  for (const engine::impact::ImpactedFile& file : prediction.files) {
    depth = std::max(depth, file.depth);
  }
  for (const engine::impact::ImpactedSymbol& symbol : prediction.symbols) {
    depth = std::max(depth, symbol.depth);
  }
  return depth;
}

void sortSnapshot(memory::StateSnapshot& snapshot) {
  std::sort(snapshot.nodes.begin(), snapshot.nodes.end(),
            [](const memory::StateNode& left, const memory::StateNode& right) {
              if (left.nodeId != right.nodeId) {
                return left.nodeId < right.nodeId;
              }
              return static_cast<int>(left.nodeType) <
                     static_cast<int>(right.nodeType);
            });

  std::sort(snapshot.edges.begin(), snapshot.edges.end(),
            [](const memory::StateEdge& left, const memory::StateEdge& right) {
              if (left.edgeId != right.edgeId) {
                return left.edgeId < right.edgeId;
              }
              if (left.sourceId != right.sourceId) {
                return left.sourceId < right.sourceId;
              }
              if (left.targetId != right.targetId) {
                return left.targetId < right.targetId;
              }
              return static_cast<int>(left.edgeType) <
                     static_cast<int>(right.edgeType);
            });

  snapshot.nodeCount = snapshot.nodes.size();
  snapshot.edgeCount = snapshot.edges.size();
  if (snapshot.snapshotId.empty()) {
    snapshot.snapshotId = std::to_string(snapshot.id);
  }
}

void appendDeterministicUseMarker(memory::StateNode& node,
                                  const std::uint64_t symbolId) {
  std::set<std::string> usedIn;
  if (node.data.contains("used_in") && node.data["used_in"].is_array()) {
    for (const auto& item : node.data["used_in"]) {
      if (item.is_string()) {
        usedIn.insert(normalizePath(item.get<std::string>()));
      }
    }
  }

  usedIn.insert("authority/simulated/" + std::to_string(symbolId));
  nlohmann::json updated = nlohmann::json::array();
  for (const std::string& path : usedIn) {
    if (!path.empty()) {
      updated.push_back(path);
    }
  }
  node.data["used_in"] = std::move(updated);
  node.data["usage_count"] = usedIn.size();
}

std::vector<std::string> collectImpactPaths(
    const engine::impact::ImpactPrediction& prediction) {
  std::set<std::string> paths;
  for (const engine::impact::ImpactedFile& file : prediction.files) {
    const std::string normalized = normalizePath(file.path);
    if (!normalized.empty()) {
      paths.insert(normalized);
    }
  }
  for (const engine::impact::ImpactedSymbol& symbol : prediction.symbols) {
    const std::string normalized = normalizePath(symbol.definedIn);
    if (!normalized.empty()) {
      paths.insert(normalized);
    }
  }
  return std::vector<std::string>(paths.begin(), paths.end());
}

bool hasFileNode(const memory::StateSnapshot& snapshot, const std::string& path) {
  const std::string nodeId = fileNodeId(path);
  return std::any_of(snapshot.nodes.begin(), snapshot.nodes.end(),
                     [&nodeId](const memory::StateNode& node) {
                       return node.nodeId == nodeId;
                     });
}

memory::StateSnapshot buildSimulatedSnapshot(
    const memory::StateSnapshot& baseline,
    const engine::impact::SimulationResult& simulation,
    const AuthorityIntentRequest& request,
    std::size_t& publicApiChangesOut) {
  memory::StateSnapshot snapshot = baseline;
  snapshot.id = baseline.id + 1U;
  snapshot.snapshotId = std::to_string(snapshot.id);

  const bool removalIntent = isRemovalIntent(request.goal);
  const bool signatureIntent = isSignatureIntent(request.goal);
  const bool dependencyIntent = isDependencyIntent(request.goal);

  std::vector<engine::impact::ImpactedSymbol> symbols =
      simulation.prediction.symbols;
  std::sort(symbols.begin(), symbols.end(),
            [](const engine::impact::ImpactedSymbol& left,
               const engine::impact::ImpactedSymbol& right) {
              if (left.isRoot != right.isRoot) {
                return left.isRoot > right.isRoot;
              }
              if (left.symbolId != right.symbolId) {
                return left.symbolId < right.symbolId;
              }
              return left.name < right.name;
            });

  if (removalIntent) {
    const std::size_t removeBudget =
        std::max<std::size_t>(1U, std::min<std::size_t>(2U, symbols.size()));
    std::size_t removed = 0U;
    for (const engine::impact::ImpactedSymbol& symbol : symbols) {
      if (symbol.symbolId == 0U) {
        continue;
      }
      const std::string nodeId = symbolNodeId(symbol.symbolId);
      const std::size_t oldNodeCount = snapshot.nodes.size();
      snapshot.nodes.erase(
          std::remove_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                         [&nodeId](const memory::StateNode& node) {
                           return node.nodeId == nodeId;
                         }),
          snapshot.nodes.end());
      if (snapshot.nodes.size() == oldNodeCount) {
        continue;
      }
      snapshot.edges.erase(
          std::remove_if(snapshot.edges.begin(), snapshot.edges.end(),
                         [&nodeId](const memory::StateEdge& edge) {
                           return edge.sourceId == nodeId ||
                                  edge.targetId == nodeId;
                         }),
          snapshot.edges.end());
      if (symbol.publicApi) {
        ++publicApiChangesOut;
      }
      ++removed;
      if (removed >= removeBudget) {
        break;
      }
    }
  }

  for (const engine::impact::ImpactedSymbol& symbol : symbols) {
    if (symbol.symbolId == 0U) {
      continue;
    }
    const std::string nodeId = symbolNodeId(symbol.symbolId);
    auto nodeIt = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
                               [&nodeId](const memory::StateNode& node) {
                                 return node.nodeId == nodeId;
                               });
    if (nodeIt == snapshot.nodes.end()) {
      continue;
    }
    if (signatureIntent) {
      const std::string name = nodeIt->data.value("name", std::string{});
      if (!name.empty()) {
        const std::string suffix =
            symbol.isRoot ? "_authority_sig" : "_authority_ref";
        if (name.find(suffix) == std::string::npos) {
          nodeIt->data["name"] = name + suffix;
        }
      }
      if (symbol.publicApi) {
        ++publicApiChangesOut;
      }
    } else {
      appendDeterministicUseMarker(*nodeIt, symbol.symbolId);
    }
  }

  if (dependencyIntent || removalIntent) {
    const std::vector<std::string> impactedPaths =
        collectImpactPaths(simulation.prediction);
    if (impactedPaths.size() >= 2U) {
      const std::string fromPath = impactedPaths[0];
      const std::string toPath = impactedPaths[1];
      if (fromPath != toPath && hasFileNode(snapshot, fromPath) &&
          hasFileNode(snapshot, toPath)) {
        const std::string fromNodeId = fileNodeId(fromPath);
        const std::string toNodeId = fileNodeId(toPath);
        if (removalIntent) {
          snapshot.edges.erase(
              std::remove_if(
                  snapshot.edges.begin(), snapshot.edges.end(),
                  [&fromNodeId, &toNodeId](const memory::StateEdge& edge) {
                    return edge.edgeType == memory::EdgeType::DependsOn &&
                           edge.sourceId == fromNodeId &&
                           edge.targetId == toNodeId;
                  }),
              snapshot.edges.end());
        } else {
          const bool alreadyExists = std::any_of(
              snapshot.edges.begin(), snapshot.edges.end(),
              [&fromNodeId, &toNodeId](const memory::StateEdge& edge) {
                return edge.edgeType == memory::EdgeType::DependsOn &&
                       edge.sourceId == fromNodeId &&
                       edge.targetId == toNodeId;
              });
          if (!alreadyExists) {
            memory::StateEdge edge;
            edge.edgeId = "dep:" + fromPath + "->" + toPath;
            edge.sourceId = fromNodeId;
            edge.targetId = toNodeId;
            edge.edgeType = memory::EdgeType::DependsOn;
            snapshot.edges.push_back(std::move(edge));
          }
        }
      }
    }
  }

  sortSnapshot(snapshot);
  return snapshot;
}

std::string selectDeterministicTarget(const ai::RuntimeState& state,
                                      const AuthorityIntentRequest& request) {
  if (!request.target.empty()) {
    return request.target;
  }
  if (!request.goal.empty()) {
    return request.goal;
  }

  std::vector<std::string> symbolNames;
  symbolNames.reserve(state.symbolIndex.size());
  for (const auto& [name, symbol] : state.symbolIndex) {
    (void)symbol;
    symbolNames.push_back(name);
  }
  std::sort(symbolNames.begin(), symbolNames.end());
  if (!symbolNames.empty()) {
    return symbolNames.front();
  }

  std::vector<std::string> paths;
  paths.reserve(state.files.size());
  for (const ai::FileRecord& file : state.files) {
    paths.push_back(normalizePath(file.path));
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::remove(paths.begin(), paths.end(), std::string{}), paths.end());
  if (!paths.empty()) {
    return paths.front();
  }
  return {};
}

}  // namespace

IntentSimulator::IntentSimulator(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()) {}

SimulatedIntentResult IntentSimulator::simulate(
    const AuthorityIntentRequest& request) const {
  core::StateManager stateManager(projectRoot_);
  std::string loadError;
  if (!stateManager.loadPersistedGraph(loadError)) {
    throw std::runtime_error(loadError.empty()
                                 ? "Unable to load persisted graph state."
                                 : loadError);
  }

  runtime::CognitiveRuntime runtime(stateManager);
  const std::size_t budget =
      request.tokenBudget == 0U ? 4096U : request.tokenBudget;
  runtime::CognitiveState state = runtime.createState(budget);
  runtime::SnapshotPinGuard pinGuard = runtime.pin(state);
  pinGuard.assertCurrent();

  if (!state.snapshot.runtimeState) {
    throw std::runtime_error("Intent simulation requires semantic runtime state.");
  }
  if (state.snapshot.graphStore == nullptr) {
    throw std::runtime_error("Intent simulation requires graph-store access.");
  }

  const std::string target = selectDeterministicTarget(*state.snapshot.runtimeState,
                                                       request);
  if (target.empty()) {
    throw std::runtime_error("Intent simulation requires a non-empty target.");
  }

  const bool symbolTarget =
      state.snapshot.runtimeState->symbolIndex.find(target) !=
      state.snapshot.runtimeState->symbolIndex.end();
  const std::size_t maxDepth = std::max<std::size_t>(1U, request.impactDepth);

  engine::impact::ImpactPredictionEngine impactEngine(
      *state.snapshot.runtimeState, state.snapshot.graphStore, state.snapshot.version);
  const engine::impact::SimulationResult simulation =
      symbolTarget ? impactEngine.simulateSymbolChange(target, maxDepth)
                   : impactEngine.simulateFileChange(target, maxDepth);

  if (!state.snapshot.graph) {
    throw std::runtime_error("Intent simulation requires a graph snapshot.");
  }
  const memory::StateSnapshot baseline =
      state.snapshot.graph->snapshot(state.snapshot.version);
  std::size_t publicApiChanges = 0U;
  const memory::StateSnapshot simulated =
      buildSimulatedSnapshot(baseline, simulation, request, publicApiChanges);

  SimulatedIntentResult result;
  result.diffReport = diff::DiffEngine::diffBranches(baseline, simulated);
  result.impactDepth =
      std::max<std::size_t>(maxDepth, computeImpactDepth(simulation.prediction));
  result.publicApiChanges = publicApiChanges;
  return result;
}

}  // namespace ultra::authority
