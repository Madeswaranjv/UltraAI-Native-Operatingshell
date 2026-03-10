#include "ExecutionGraph.h"
#include <algorithm>
#include <queue>

namespace ultra::intelligence {

void ExecutionGraph::addNode(const ExecutionNode& node) {
  nodes_[node.nodeId] = node;
}

ExecutionNode ExecutionGraph::getNode(const std::string& nodeId) const {
  auto it = nodes_.find(nodeId);
  if (it != nodes_.end()) return it->second;
  return ExecutionNode{};
}

void ExecutionGraph::addDependency(const std::string& sourceId, const std::string& targetId) {
  auto& outList = outboundMap_[sourceId];
  if (std::find(outList.begin(), outList.end(), targetId) == outList.end()) {
    outList.push_back(targetId);
  }
  
  auto& inList = inboundMap_[targetId];
  if (std::find(inList.begin(), inList.end(), sourceId) == inList.end()) {
    inList.push_back(sourceId);
  }
}

std::vector<std::string> ExecutionGraph::getActivePath() const {
  std::vector<std::string> activeNodes;
  for (const auto& [id, node] : nodes_) {
    if (node.status == NodeStatus::Running) {
      activeNodes.push_back(id);
    }
  }
  return activeNodes;
}

std::vector<ExecutionNode> ExecutionGraph::getNodesByBranch(const std::string& branchId) const {
  std::vector<ExecutionNode> branchNodes;
  for (const auto& [id, node] : nodes_) {
    if (node.branchId == branchId) {
      branchNodes.push_back(node);
    }
  }
  return branchNodes;
}

std::vector<std::string> ExecutionGraph::getDependencies(const std::string& nodeId) const {
  auto it = inboundMap_.find(nodeId);
  if (it != inboundMap_.end()) {
    return it->second;
  }
  return {};
}

std::vector<std::string> ExecutionGraph::topologicalOrder() const {
  std::vector<std::string> order;
  std::map<std::string, int> inDegree;
  
  // Initialize in-degrees
  for (const auto& [id, _] : nodes_) {
    inDegree[id] = 0;
  }
  
  // Calculate in-degrees
  for (const auto& [source, targets] : outboundMap_) {
    for (const auto& target : targets) {
      if (nodes_.count(target)) { // Only count if target exists
        inDegree[target]++;
      }
    }
  }
  
  std::queue<std::string> zeros;
  for (const auto& [id, deg] : inDegree) {
    if (deg == 0) zeros.push(id);
  }
  
  while (!zeros.empty()) {
    std::string current = zeros.front();
    zeros.pop();
    order.push_back(current);
    
    auto outIt = outboundMap_.find(current);
    if (outIt != outboundMap_.end()) {
      for (const auto& target : outIt->second) {
        if (nodes_.count(target)) {
          inDegree[target]--;
          if (inDegree[target] == 0) {
            zeros.push(target);
          }
        }
      }
    }
  }
  
  if (order.size() != nodes_.size()) {
    return std::vector<std::string>{}; // Cycle detected
  }
  return order;
}

void ExecutionGraph::clear() {
  nodes_.clear();
  outboundMap_.clear();
  inboundMap_.clear();
}

}  // namespace ultra::intelligence
