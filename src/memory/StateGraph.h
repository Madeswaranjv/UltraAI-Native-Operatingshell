#pragma once

#include "StateEdge.h"
#include "StateNode.h"
#include "StateSnapshot.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ultra::memory {

class StateGraph;

struct GraphOverlay {
  std::shared_ptr<const StateGraph> parentSnapshot;
  std::map<std::string, StateNode> addedNodes;
  std::map<std::string, StateNode> modifiedNodes;
  std::set<std::string> removedNodes;
  std::map<std::string, StateEdge> addedEdges;
  std::map<std::string, StateEdge> modifiedEdges;
  std::set<std::string> removedEdges;

  [[nodiscard]] bool empty() const noexcept;
};

class StateGraphBuilder {
 public:
  void putNode(const StateNode& node);
  void putEdge(const StateEdge& edge);

  [[nodiscard]] StateGraph build() const;
  [[nodiscard]] std::shared_ptr<const StateGraph> buildShared() const;

 private:
  std::map<std::string, StateNode> nodes_;
  std::map<std::string, StateEdge> edges_;
};

class StateGraph {
 public:
  StateGraph();

  [[nodiscard]] static StateGraph fromSnapshot(const StateSnapshot& snapshot);
  [[nodiscard]] static std::shared_ptr<const StateGraph> createOverlaySnapshot(
      const std::shared_ptr<const StateGraph>& parentSnapshot,
      GraphOverlay overlay);

  // Compatibility shims for legacy single-owner callers. The runtime core does
  // not mutate shared graph snapshots through these APIs.
  void addNode(const StateNode& node);
  bool removeNode(const std::string& nodeId);
  void addEdge(const StateEdge& edge);
  bool removeEdge(const std::string& edgeId);
  void restore(const StateSnapshot& snapshot);

  [[nodiscard]] StateNode getNode(const std::string& nodeId) const;
  [[nodiscard]] StateEdge getEdge(const std::string& edgeId) const;

  [[nodiscard]] std::vector<StateEdge> getOutboundEdges(
      const std::string& sourceId) const;
  [[nodiscard]] std::vector<StateNode> queryByType(NodeType type) const;

  [[nodiscard]] StateSnapshot snapshot(std::uint64_t snapshotId) const;
  [[nodiscard]] std::size_t nodeCount() const;
  [[nodiscard]] std::size_t edgeCount() const;
  [[nodiscard]] std::string getDeterministicHash() const;

 private:
  struct GraphStorage {
    std::map<std::string, StateNode> nodes;
    std::map<std::string, StateEdge> edges;
  };

  explicit StateGraph(std::shared_ptr<const GraphStorage> storage,
                      std::shared_ptr<const GraphOverlay> overlay);

  void collectResolvedNodes(std::map<std::string, StateNode>& nodes) const;
  void collectResolvedEdges(std::map<std::string, StateEdge>& edges) const;
  static void pruneDanglingEdges(
      const std::map<std::string, StateNode>& nodes,
      std::map<std::string, StateEdge>& edges);
  static std::string computeHash(
      const std::map<std::string, StateNode>& nodes,
      const std::map<std::string, StateEdge>& edges);

  std::shared_ptr<const GraphStorage> storage_;
  std::shared_ptr<const GraphOverlay> overlay_;

  friend class StateGraphBuilder;
};

}  // namespace ultra::memory
