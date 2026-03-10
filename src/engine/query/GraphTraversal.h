#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <queue>
#include <set>
#include <utility>
#include <vector>

namespace ultra::engine::query {

class GraphTraversal {
 public:
  enum class Direction : std::uint8_t { Forward = 0U, Reverse = 1U };

  template <typename NodeId>
  struct Plan {
    std::vector<NodeId> startNodes;
    Direction direction{Direction::Forward};
    std::size_t maxDepth{1U};
    bool includeStartNodes{false};
  };

  template <typename NodeId>
  struct Result {
    std::vector<NodeId> orderedNodes;
    std::map<NodeId, std::size_t> depthByNode;
  };

  template <typename NodeId>
  static Result<NodeId> execute(
      const Plan<NodeId>& plan,
      const std::map<NodeId, std::vector<NodeId>>& forwardAdjacency,
      const std::map<NodeId, std::vector<NodeId>>& reverseAdjacency) {
    Result<NodeId> result;
    if (plan.startNodes.empty()) {
      return result;
    }

    std::vector<NodeId> startNodes = plan.startNodes;
    std::sort(startNodes.begin(), startNodes.end());
    startNodes.erase(std::unique(startNodes.begin(), startNodes.end()),
                     startNodes.end());

    const auto& adjacency =
        plan.direction == Direction::Forward ? forwardAdjacency : reverseAdjacency;

    std::set<NodeId> visited;
    std::queue<std::pair<NodeId, std::size_t>> frontier;

    for (const NodeId start : startNodes) {
      if (!visited.insert(start).second) {
        continue;
      }
      frontier.push({start, 0U});
      if (plan.includeStartNodes) {
        result.orderedNodes.push_back(start);
        result.depthByNode[start] = 0U;
      }
    }

    while (!frontier.empty()) {
      const NodeId current = frontier.front().first;
      const std::size_t depth = frontier.front().second;
      frontier.pop();

      if (depth >= plan.maxDepth) {
        continue;
      }

      const auto adjIt = adjacency.find(current);
      if (adjIt == adjacency.end()) {
        continue;
      }

      for (const NodeId neighbor : adjIt->second) {
        if (!visited.insert(neighbor).second) {
          continue;
        }

        const std::size_t nextDepth = depth + 1U;
        result.orderedNodes.push_back(neighbor);
        result.depthByNode[neighbor] = nextDepth;
        frontier.push({neighbor, nextDepth});
      }
    }

    return result;
  }
};

}  // namespace ultra::engine::query

