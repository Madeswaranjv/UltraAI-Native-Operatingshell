#include "StateGraph.h"
#include "../ai/Hashing.h"
#include "epoch/EpochGuard.h"
#include "../metrics/PerformanceMetrics.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace ultra::memory {

namespace {

std::size_t estimateSnapshotSizeBytes(const StateSnapshot& snapshot) {
  std::size_t total =
      sizeof(snapshot.id) + sizeof(snapshot.nodeCount) +
      sizeof(snapshot.edgeCount) + snapshot.snapshotId.size() +
      snapshot.graphHash.size();

  for (const StateNode& node : snapshot.nodes) {
    total += sizeof(node.nodeType) + sizeof(node.version) + node.nodeId.size() +
             node.data.dump().size();
  }
  for (const StateEdge& edge : snapshot.edges) {
    total += sizeof(edge.edgeType) + sizeof(edge.weight) + edge.edgeId.size() +
             edge.sourceId.size() + edge.targetId.size();
  }
  return total;
}

}  // namespace

void StateGraph::addNode(const StateNode& node) {
  auto it = nodes_.find(node.nodeId);
  if (it != nodes_.end()) {
    StateNode updated = node;
    updated.version = it->second.version + 1;
    nodes_[node.nodeId] = std::move(updated);
  } else {
    nodes_[node.nodeId] = node;
  }
}

bool StateGraph::removeNode(const std::string& nodeId) {
  if (nodes_.erase(nodeId) == 0)
    return false;

  auto outIt = outboundEdgeMap_.find(nodeId);
  if (outIt != outboundEdgeMap_.end()) {
    for (const auto& edgeId : outIt->second)
      edges_.erase(edgeId);
    outboundEdgeMap_.erase(outIt);
  }

  for (auto it = edges_.begin(); it != edges_.end();) {
    if (it->second.targetId == nodeId)
      it = edges_.erase(it);
    else
      ++it;
  }

  return true;
}

StateNode StateGraph::getNode(const std::string& nodeId) const {
  epoch::EpochGuard guard(epoch::EpochManager::instance());
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end())
    return it->second;
  return StateNode{};
}

void StateGraph::addEdge(const StateEdge& edge) {
  edges_[edge.edgeId] = edge;
  outboundEdgeMap_[edge.sourceId].push_back(edge.edgeId);
}

bool StateGraph::removeEdge(const std::string& edgeId) {
  auto it = edges_.find(edgeId);
  if (it == edges_.end())
    return false;

  std::string sourceId = it->second.sourceId;
  edges_.erase(it);

  auto& vec = outboundEdgeMap_[sourceId];
  vec.erase(std::remove(vec.begin(), vec.end(), edgeId), vec.end());
  return true;
}

std::vector<StateEdge>
StateGraph::getOutboundEdges(
    const std::string& sourceId) const {
  epoch::EpochGuard guard(epoch::EpochManager::instance());

  std::vector<StateEdge> result;

  auto it = outboundEdgeMap_.find(sourceId);
  if (it != outboundEdgeMap_.end()) {
    for (const auto& edgeId : it->second) {
      auto eIt = edges_.find(edgeId);
      if (eIt != edges_.end())
        result.push_back(eIt->second);
    }

    std::sort(result.begin(), result.end(),
              [](const StateEdge& a,
                 const StateEdge& b) {
                return a.edgeId < b.edgeId;
              });
  }

  return result;
}

std::vector<StateNode>
StateGraph::queryByType(NodeType type) const {
  epoch::EpochGuard guard(epoch::EpochManager::instance());

  std::vector<StateNode> result;

  std::vector<std::string> keys;
  for (const auto& kv : nodes_)
    keys.push_back(kv.first);

  std::sort(keys.begin(), keys.end());

  for (const auto& k : keys) {
    if (nodes_.at(k).nodeType == type)
      result.push_back(nodes_.at(k));
  }

  return result;
}

StateSnapshot
StateGraph::snapshot(uint64_t snapshotId) const {
  epoch::EpochGuard guard(epoch::EpochManager::instance());
  const bool metricsEnabled = metrics::PerformanceMetrics::isEnabled();
  const auto startedAt =
      metricsEnabled ? std::chrono::steady_clock::now()
                     : std::chrono::steady_clock::time_point{};

  StateSnapshot snap;
  snap.id = snapshotId;
  snap.snapshotId = std::to_string(snapshotId);

  std::vector<std::string> nodeKeys;
  for (const auto& kv : nodes_)
    nodeKeys.push_back(kv.first);
  std::sort(nodeKeys.begin(), nodeKeys.end());

  for (const auto& k : nodeKeys)
    snap.nodes.push_back(nodes_.at(k));

  std::vector<std::string> edgeKeys;
  for (const auto& kv : edges_)
    edgeKeys.push_back(kv.first);
  std::sort(edgeKeys.begin(), edgeKeys.end());

  for (const auto& k : edgeKeys)
    snap.edges.push_back(edges_.at(k));

  snap.nodeCount = snap.nodes.size();
  snap.edgeCount = snap.edges.size();
  snap.graphHash = computeHash();

  if (metricsEnabled) {
    metrics::SnapshotMetrics metric;
    metric.operation = "state_graph_snapshot";
    metric.durationMicros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt)
            .count());
    metric.nodeCount = snap.nodeCount;
    metric.edgeCount = snap.edgeCount;
    metric.snapshotSizeBytes = estimateSnapshotSizeBytes(snap);
    metrics::PerformanceMetrics::recordSnapshotMetric(metric);
  }

  return snap;
}

void StateGraph::restore(const StateSnapshot& snapshot) {

  nodes_.clear();
  edges_.clear();
  outboundEdgeMap_.clear();

  for (const auto& n : snapshot.nodes)
    nodes_[n.nodeId] = n;

  for (const auto& e : snapshot.edges) {
    edges_[e.edgeId] = e;
    outboundEdgeMap_[e.sourceId].push_back(e.edgeId);
  }
}

std::string StateGraph::getDeterministicHash() const {
  epoch::EpochGuard guard(epoch::EpochManager::instance());
  return computeHash();
}

std::string StateGraph::computeHash() const {

  ultra::ai::Sha256Accumulator acc;

  std::vector<std::string> nodeKeys;
  for (const auto& kv : nodes_)
    nodeKeys.push_back(kv.first);
  std::sort(nodeKeys.begin(), nodeKeys.end());

  for (const auto& id : nodeKeys) {
    const auto& n = nodes_.at(id);
    acc.update("node:");
    acc.update(n.nodeId);
  }

  std::vector<std::string> edgeKeys;
  for (const auto& kv : edges_)
    edgeKeys.push_back(kv.first);
  std::sort(edgeKeys.begin(), edgeKeys.end());

  for (const auto& id : edgeKeys) {
    const auto& e = edges_.at(id);
    acc.update("edge:");
    acc.update(e.edgeId);
  }

  return ultra::ai::hashToHex(acc.finalize());
}

}  // namespace ultra::memory
