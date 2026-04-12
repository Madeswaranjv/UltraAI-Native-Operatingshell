#include "StateGraph.h"

#include "../ai/Hashing.h"
#include "../metrics/PerformanceMetrics.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ultra::memory {

namespace {

std::size_t estimateSnapshotSizeBytes(const StateSnapshot& snapshot) {
  std::size_t total =
      sizeof(snapshot.id) + sizeof(snapshot.nodeCount) +
      sizeof(snapshot.edgeCount) + snapshot.snapshotId.size() +
      snapshot.graphHash.size() + sizeof(snapshot.createdAt) +
      snapshot.branchId.size();

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

bool GraphOverlay::empty() const noexcept {
  return addedNodes.empty() && modifiedNodes.empty() && removedNodes.empty() &&
         addedEdges.empty() && modifiedEdges.empty() && removedEdges.empty();
}

void StateGraphBuilder::putNode(const StateNode& node) {
  nodes_[node.nodeId] = node;
}

void StateGraphBuilder::putEdge(const StateEdge& edge) {
  edges_[edge.edgeId] = edge;
}

StateGraph StateGraphBuilder::build() const {
  auto storage = std::make_shared<StateGraph::GraphStorage>();
  storage->nodes = nodes_;
  storage->edges = edges_;
  StateGraph::pruneDanglingEdges(storage->nodes, storage->edges);
  return StateGraph(std::move(storage), nullptr);
}

std::shared_ptr<const StateGraph> StateGraphBuilder::buildShared() const {
  return std::make_shared<const StateGraph>(build());
}

StateGraph::StateGraph()
    : storage_(std::make_shared<GraphStorage>()), overlay_(nullptr) {}

StateGraph::StateGraph(std::shared_ptr<const GraphStorage> storage,
                       std::shared_ptr<const GraphOverlay> overlay)
    : storage_(std::move(storage)), overlay_(std::move(overlay)) {
  if (!storage_ && !overlay_) {
    storage_ = std::make_shared<GraphStorage>();
  }
}

StateGraph StateGraph::fromSnapshot(const StateSnapshot& snapshot) {
  StateGraphBuilder builder;
  for (const StateNode& node : snapshot.nodes) {
    builder.putNode(node);
  }
  for (const StateEdge& edge : snapshot.edges) {
    builder.putEdge(edge);
  }
  return builder.build();
}

std::shared_ptr<const StateGraph> StateGraph::createOverlaySnapshot(
    const std::shared_ptr<const StateGraph>& parentSnapshot,
    GraphOverlay overlay) {
  if (!parentSnapshot) {
    return std::make_shared<const StateGraph>();
  }

  overlay.parentSnapshot = parentSnapshot;
  if (overlay.empty()) {
    return parentSnapshot;
  }

  return std::shared_ptr<const StateGraph>(new StateGraph(
      std::shared_ptr<const GraphStorage>{},
      std::make_shared<const GraphOverlay>(std::move(overlay))));
}

void StateGraph::addNode(const StateNode& node) {
  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);

  auto updated = node;
  const auto existingIt = nodes.find(node.nodeId);
  if (existingIt != nodes.end()) {
    updated.version = existingIt->second.version + 1U;
  }
  nodes[node.nodeId] = std::move(updated);
  pruneDanglingEdges(nodes, edges);

  auto storage = std::make_shared<GraphStorage>();
  storage->nodes = std::move(nodes);
  storage->edges = std::move(edges);
  storage_ = std::move(storage);
  overlay_.reset();
}

bool StateGraph::removeNode(const std::string& nodeId) {
  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);

  if (nodes.erase(nodeId) == 0U) {
    return false;
  }
  for (auto it = edges.begin(); it != edges.end();) {
    if (it->second.sourceId == nodeId || it->second.targetId == nodeId) {
      it = edges.erase(it);
      continue;
    }
    ++it;
  }

  auto storage = std::make_shared<GraphStorage>();
  storage->nodes = std::move(nodes);
  storage->edges = std::move(edges);
  storage_ = std::move(storage);
  overlay_.reset();
  return true;
}

void StateGraph::addEdge(const StateEdge& edge) {
  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);
  edges[edge.edgeId] = edge;
  pruneDanglingEdges(nodes, edges);

  auto storage = std::make_shared<GraphStorage>();
  storage->nodes = std::move(nodes);
  storage->edges = std::move(edges);
  storage_ = std::move(storage);
  overlay_.reset();
}

bool StateGraph::removeEdge(const std::string& edgeId) {
  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);

  if (edges.erase(edgeId) == 0U) {
    return false;
  }

  auto storage = std::make_shared<GraphStorage>();
  storage->nodes = std::move(nodes);
  storage->edges = std::move(edges);
  storage_ = std::move(storage);
  overlay_.reset();
  return true;
}

void StateGraph::restore(const StateSnapshot& snapshot) {
  *this = fromSnapshot(snapshot);
}

StateNode StateGraph::getNode(const std::string& nodeId) const {
  if (overlay_) {
    if (overlay_->removedNodes.find(nodeId) != overlay_->removedNodes.end()) {
      return {};
    }
    const auto modifiedIt = overlay_->modifiedNodes.find(nodeId);
    if (modifiedIt != overlay_->modifiedNodes.end()) {
      return modifiedIt->second;
    }
    const auto addedIt = overlay_->addedNodes.find(nodeId);
    if (addedIt != overlay_->addedNodes.end()) {
      return addedIt->second;
    }
    if (overlay_->parentSnapshot) {
      return overlay_->parentSnapshot->getNode(nodeId);
    }
    return {};
  }

  const auto it = storage_->nodes.find(nodeId);
  if (it == storage_->nodes.end()) {
    return {};
  }
  return it->second;
}

StateEdge StateGraph::getEdge(const std::string& edgeId) const {
  if (overlay_) {
    if (overlay_->removedEdges.find(edgeId) != overlay_->removedEdges.end()) {
      return {};
    }
    const auto modifiedIt = overlay_->modifiedEdges.find(edgeId);
    if (modifiedIt != overlay_->modifiedEdges.end()) {
      return modifiedIt->second;
    }
    const auto addedIt = overlay_->addedEdges.find(edgeId);
    if (addedIt != overlay_->addedEdges.end()) {
      return addedIt->second;
    }
    if (overlay_->parentSnapshot) {
      return overlay_->parentSnapshot->getEdge(edgeId);
    }
    return {};
  }

  const auto it = storage_->edges.find(edgeId);
  if (it == storage_->edges.end()) {
    return {};
  }
  return it->second;
}

std::vector<StateEdge> StateGraph::getOutboundEdges(
    const std::string& sourceId) const {
  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);
  pruneDanglingEdges(nodes, edges);

  if (nodes.find(sourceId) == nodes.end()) {
    return {};
  }

  std::vector<StateEdge> result;
  for (const auto& [edgeId, edge] : edges) {
    (void)edgeId;
    if (edge.sourceId == sourceId) {
      result.push_back(edge);
    }
  }
  return result;
}

std::vector<StateNode> StateGraph::queryByType(const NodeType type) const {
  std::map<std::string, StateNode> nodes;
  collectResolvedNodes(nodes);

  std::vector<StateNode> result;
  for (const auto& [nodeId, node] : nodes) {
    (void)nodeId;
    if (node.nodeType == type) {
      result.push_back(node);
    }
  }
  return result;
}

StateSnapshot StateGraph::snapshot(const std::uint64_t snapshotId) const {
  const bool metricsEnabled = metrics::PerformanceMetrics::isEnabled();
  const auto startedAt =
      metricsEnabled ? std::chrono::steady_clock::now()
                     : std::chrono::steady_clock::time_point{};

  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);
  pruneDanglingEdges(nodes, edges);

  StateSnapshot snapshot;
  snapshot.id = snapshotId;
  snapshot.snapshotId = std::to_string(snapshotId);
  snapshot.createdAt = ultra::types::Timestamp::now();
  snapshot.nodeCount = nodes.size();
  snapshot.edgeCount = edges.size();
  snapshot.graphHash = computeHash(nodes, edges);

  snapshot.nodes.reserve(nodes.size());
  for (const auto& [nodeId, node] : nodes) {
    (void)nodeId;
    snapshot.nodes.push_back(node);
  }

  snapshot.edges.reserve(edges.size());
  for (const auto& [edgeId, edge] : edges) {
    (void)edgeId;
    snapshot.edges.push_back(edge);
  }

  if (metricsEnabled) {
    metrics::SnapshotMetrics metric;
    metric.operation = "state_graph_snapshot";
    metric.durationMicros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt)
            .count());
    metric.nodeCount = snapshot.nodeCount;
    metric.edgeCount = snapshot.edgeCount;
    metric.snapshotSizeBytes = estimateSnapshotSizeBytes(snapshot);
    metrics::PerformanceMetrics::recordSnapshotMetric(metric);
  }

  return snapshot;
}

std::size_t StateGraph::nodeCount() const {
  std::map<std::string, StateNode> nodes;
  collectResolvedNodes(nodes);
  return nodes.size();
}

std::size_t StateGraph::edgeCount() const {
  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);
  pruneDanglingEdges(nodes, edges);
  return edges.size();
}

std::string StateGraph::getDeterministicHash() const {
  std::map<std::string, StateNode> nodes;
  std::map<std::string, StateEdge> edges;
  collectResolvedNodes(nodes);
  collectResolvedEdges(edges);
  pruneDanglingEdges(nodes, edges);
  return computeHash(nodes, edges);
}

void StateGraph::collectResolvedNodes(std::map<std::string, StateNode>& nodes) const {
  if (overlay_) {
    if (overlay_->parentSnapshot) {
      overlay_->parentSnapshot->collectResolvedNodes(nodes);
    }

    for (const std::string& nodeId : overlay_->removedNodes) {
      nodes.erase(nodeId);
    }
    for (const auto& [nodeId, node] : overlay_->modifiedNodes) {
      nodes[nodeId] = node;
    }
    for (const auto& [nodeId, node] : overlay_->addedNodes) {
      nodes[nodeId] = node;
    }
    return;
  }

  nodes = storage_->nodes;
}

void StateGraph::collectResolvedEdges(std::map<std::string, StateEdge>& edges) const {
  if (overlay_) {
    if (overlay_->parentSnapshot) {
      overlay_->parentSnapshot->collectResolvedEdges(edges);
    }

    for (const std::string& edgeId : overlay_->removedEdges) {
      edges.erase(edgeId);
    }
    for (const auto& [edgeId, edge] : overlay_->modifiedEdges) {
      edges[edgeId] = edge;
    }
    for (const auto& [edgeId, edge] : overlay_->addedEdges) {
      edges[edgeId] = edge;
    }
    return;
  }

  edges = storage_->edges;
}

void StateGraph::pruneDanglingEdges(
    const std::map<std::string, StateNode>& nodes,
    std::map<std::string, StateEdge>& edges) {
  for (auto it = edges.begin(); it != edges.end();) {
    if (nodes.find(it->second.sourceId) == nodes.end() ||
        nodes.find(it->second.targetId) == nodes.end()) {
      it = edges.erase(it);
      continue;
    }
    ++it;
  }
}

std::string StateGraph::computeHash(
    const std::map<std::string, StateNode>& nodes,
    const std::map<std::string, StateEdge>& edges) {
  ultra::ai::Sha256Accumulator accumulator;

  for (const auto& [nodeId, node] : nodes) {
    accumulator.update("node:");
    accumulator.update(nodeId);
    accumulator.update("|type:");
    accumulator.update(std::to_string(static_cast<int>(node.nodeType)));
    accumulator.update("|version:");
    accumulator.update(std::to_string(node.version));
    accumulator.update("|data:");
    accumulator.update(node.data.dump());
  }

  for (const auto& [edgeId, edge] : edges) {
    accumulator.update("edge:");
    accumulator.update(edgeId);
    accumulator.update("|source:");
    accumulator.update(edge.sourceId);
    accumulator.update("|target:");
    accumulator.update(edge.targetId);
    accumulator.update("|type:");
    accumulator.update(std::to_string(static_cast<int>(edge.edgeType)));
    accumulator.update("|weight:");
    accumulator.update(std::to_string(edge.weight));
  }

  return ultra::ai::hashToHex(accumulator.finalize());
}

}  // namespace ultra::memory
