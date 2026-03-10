#pragma once

#include "StateNode.h"
#include "StateEdge.h"
#include "StateSnapshot.h"
#include "epoch/EpochGuard.h"

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace ultra::memory {

class StateGraph {
 public:
  StateGraph() = default;

  void addNode(const StateNode& node);
  bool removeNode(const std::string& nodeId);
  StateNode getNode(const std::string& nodeId) const;

  void addEdge(const StateEdge& edge);
  bool removeEdge(const std::string& edgeId);

  std::vector<StateEdge> getOutboundEdges(
      const std::string& sourceId) const;

  std::vector<StateNode> queryByType(NodeType type) const;

  StateSnapshot snapshot(uint64_t snapshotId) const;
  void restore(const StateSnapshot& snapshot);

  std::size_t nodeCount() const {
    epoch::EpochGuard guard(epoch::EpochManager::instance());
    return nodes_.size();
  }

  std::size_t edgeCount() const {
    epoch::EpochGuard guard(epoch::EpochManager::instance());
    return edges_.size();
  }

  std::string getDeterministicHash() const;

 private:
  std::string computeHash() const;

  std::unordered_map<std::string, StateNode> nodes_;
  std::unordered_map<std::string, StateEdge> edges_;
  std::unordered_map<std::string,
                     std::vector<std::string>> outboundEdgeMap_;
};

}  // namespace ultra::memory
