#include "TaskGraph.h"

#include <algorithm>
#include <queue>

namespace ultra::orchestration {

void TaskGraph::addNode(const SubTask& task) {
  nodes_[task.taskId] = task;
}

void TaskGraph::addDependency(const std::string& sourceId, const std::string& targetId) {
  auto& outList = outboundMap_[sourceId];
  if (std::find(outList.begin(), outList.end(), targetId) == outList.end()) {
    outList.push_back(targetId);
    std::sort(outList.begin(), outList.end());
  }
  
  auto& inList = inboundMap_[targetId];
  if (std::find(inList.begin(), inList.end(), sourceId) == inList.end()) {
    inList.push_back(sourceId);
    std::sort(inList.begin(), inList.end());
  }
}

SubTask TaskGraph::getNode(const std::string& taskId) const {
  auto it = nodes_.find(taskId);
  if (it != nodes_.end()) return it->second;
  return SubTask{};
}

std::vector<SubTask> TaskGraph::getAllNodes() const {
  std::vector<SubTask> result;
  result.reserve(nodes_.size());
  for (const auto& [id, task] : nodes_) {
    result.push_back(task);
  }
  return result;
}

std::vector<std::string> TaskGraph::topologicalOrder() const {
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

}  // namespace ultra::orchestration
