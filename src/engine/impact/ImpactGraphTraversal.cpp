#include "ImpactGraphTraversal.h"

namespace ultra::engine::impact {

template <typename NodeT>
typename ImpactGraphTraversal::Result<NodeT>
ImpactGraphTraversal::traverse(
    const Request<NodeT>& request,
    const std::map<NodeT, std::vector<NodeT>>& forwardAdj,
    const std::map<NodeT, std::vector<NodeT>>& reverseAdj) {

  Result<NodeT> result;

  if (request.startNodes.empty()) {
    return result;
  }

  std::set<NodeT> allowedSet(
      request.allowedNodes.begin(),
      request.allowedNodes.end());

  std::set<NodeT> visited;

  std::queue<std::pair<NodeT, std::size_t>> queue;

  for (const NodeT start : request.startNodes) {
    visited.insert(start);
    queue.emplace(start, 0);
  }

  while (!queue.empty()) {
    const auto [node, depth] = queue.front();
    queue.pop();

    if (depth > request.maxDepth) {
      continue;
    }

    const bool isStartNode =
        std::find(request.startNodes.begin(),
                  request.startNodes.end(),
                  node) != request.startNodes.end();

    const bool shouldEmit =
        !(isStartNode && !request.includeStartNodes) &&
        (allowedSet.empty() || allowedSet.count(node));

    if (shouldEmit) {
      if (result.depthByNode.find(node) == result.depthByNode.end()) {
        result.depthByNode[node] = depth;
        result.orderedNodes.push_back(node);

        if (result.orderedNodes.size() >= request.maxNodes) {
          break;
        }
      }
    }

    if (depth == request.maxDepth) {
      continue;
    }

    const std::map<NodeT, std::vector<NodeT>>* adj = nullptr;

    if (request.direction == TraversalDirection::Forward) {
      adj = &forwardAdj;
    } else {
      adj = &reverseAdj;
    }

    auto it = adj->find(node);
    if (it == adj->end()) {
      continue;
    }

    const std::vector<NodeT>& neighbors = it->second;

    for (const NodeT neighbor : neighbors) {
      if (visited.insert(neighbor).second) {
        queue.emplace(neighbor, depth + 1);
      }
    }
  }

  return result;
}

// Explicit template instantiations
template ImpactGraphTraversal::Result<std::uint32_t>
ImpactGraphTraversal::traverse(
    const Request<std::uint32_t>&,
    const std::map<std::uint32_t, std::vector<std::uint32_t>>&,
    const std::map<std::uint32_t, std::vector<std::uint32_t>>&);

template ImpactGraphTraversal::Result<std::uint64_t>
ImpactGraphTraversal::traverse(
    const Request<std::uint64_t>&,
    const std::map<std::uint64_t, std::vector<std::uint64_t>>&,
    const std::map<std::uint64_t, std::vector<std::uint64_t>>&);

}  // namespace ultra::engine::impact