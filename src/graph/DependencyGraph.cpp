#include "DependencyGraph.h"
#include <algorithm>
#include <queue>
#include <set>

namespace ultra::graph {

void DependencyGraph::addNode(const std::string& file) {
  adjacency_.try_emplace(file, std::vector<std::string>{});
}

void DependencyGraph::addEdge(const std::string& from, const std::string& to) {
  addNode(from);
  addNode(to);
  std::vector<std::string>& out = adjacency_.at(from);
  if (std::ranges::find(out, to) == out.end()) {
    out.push_back(to);
    ++edgeCount_;
  }
}

void DependencyGraph::addDependency(const std::string& from,
                                    const std::string& to) {
  addEdge(from, to);
}

void DependencyGraph::removeEdge(const std::string& from,
                                 const std::string& to) {
  auto fromIt = adjacency_.find(from);
  if (fromIt == adjacency_.end()) {
    return;
  }

  std::vector<std::string>& out = fromIt->second;
  const auto oldSize = out.size();
  out.erase(std::remove(out.begin(), out.end(), to), out.end());
  edgeCount_ -= oldSize - out.size();
}

void DependencyGraph::removeDependency(const std::string& from,
                                       const std::string& to) {
  removeEdge(from, to);
}

void DependencyGraph::removeNode(const std::string& file) {
  auto nodeIt = adjacency_.find(file);
  if (nodeIt != adjacency_.end()) {
    edgeCount_ -= nodeIt->second.size();
    adjacency_.erase(nodeIt);
  }

  for (auto& [from, out] : adjacency_) {
    (void)from;
    const auto oldSize = out.size();
    out.erase(std::remove(out.begin(), out.end(), file), out.end());
    edgeCount_ -= oldSize - out.size();
  }
}

void DependencyGraph::updateNode(const std::string& file,
                                 const std::vector<std::string>& dependencies) {
  addNode(file);
  std::vector<std::string>& out = adjacency_.at(file);
  edgeCount_ -= out.size();
  out.clear();

  for (const std::string& dependency : dependencies) {
    addNode(dependency);
    if (std::ranges::find(out, dependency) == out.end()) {
      out.push_back(dependency);
    }
  }
  edgeCount_ += out.size();
}

bool DependencyGraph::hasCycleFrom(
    const std::string& node,
    std::unordered_map<std::string, VisitState>& state) const {
  auto it = state.find(node);
  if (it != state.end()) {
    if (it->second == VisitState::Visiting) return true;
    return false;
  }
  state[node] = VisitState::Visiting;
  auto adjIt = adjacency_.find(node);
  if (adjIt != adjacency_.end()) {
    for (const std::string& neighbor : adjIt->second) {
      if (hasCycleFrom(neighbor, state)) return true;
    }
  }
  state[node] = VisitState::Visited;
  return false;
}

bool DependencyGraph::hasCycle() const {
  std::unordered_map<std::string, VisitState> state;
  for (const auto& [node, _] : adjacency_) {
    if (state.count(node) == 0 && hasCycleFrom(node, state)) return true;
  }
  return false;
}

std::vector<std::string> DependencyGraph::topologicalSort() const {
  if (hasCycle()) return {};
  std::unordered_map<std::string, std::size_t> inDegree;
  for (const auto& [node, _] : adjacency_) {
    inDegree.try_emplace(node, 0);
  }
  for (const auto& [from, toList] : adjacency_) {
    for (const std::string& to : toList) {
      ++inDegree[to];
    }
  }

  // Use a sorted set to ensure deterministic selection of zero in-degree nodes
  std::set<std::string> zeros;
  for (const auto& [node, deg] : inDegree) {
    if (deg == 0) zeros.insert(node);
  }

  std::vector<std::string> order;
  order.reserve(adjacency_.size());
  while (!zeros.empty()) {
    auto it = zeros.begin();
    std::string n = *it;
    zeros.erase(it);
    order.push_back(n);
    auto adjIt = adjacency_.find(n);
    if (adjIt != adjacency_.end()) {
      for (const std::string& neighbor : adjIt->second) {
        auto dIt = inDegree.find(neighbor);
        if (dIt != inDegree.end() && dIt->second > 0) {
          --dIt->second;
          if (dIt->second == 0) zeros.insert(neighbor);
        }
      }
    }
  }
  if (order.size() != adjacency_.size()) return {};
  return order;
}

std::vector<std::string> DependencyGraph::getDependencies(
    const std::string& file) const {
  auto it = adjacency_.find(file);
  if (it == adjacency_.end()) return {};
  auto res = it->second;
  std::sort(res.begin(), res.end());
  return res;
}

std::vector<std::string> DependencyGraph::getNodes() const {
  std::vector<std::string> nodes;
  nodes.reserve(adjacency_.size());
  for (const auto& [node, _] : adjacency_) {
    nodes.push_back(node);
  }
  std::sort(nodes.begin(), nodes.end());
  return nodes;
}

std::size_t DependencyGraph::nodeCount() const noexcept {
  return adjacency_.size();
}

std::size_t DependencyGraph::edgeCount() const noexcept {
  return edgeCount_;
}

}  // namespace ultra::graph
