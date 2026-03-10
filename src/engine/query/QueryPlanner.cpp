#include "QueryPlanner.h"

namespace ultra::engine::query {

GraphTraversal::Plan<std::uint32_t> QueryPlanner::planFileDependencyQuery(
    const std::optional<std::uint32_t>& fileId) const {
  GraphTraversal::Plan<std::uint32_t> plan;
  if (fileId.has_value()) {
    plan.startNodes.push_back(*fileId);
  }
  plan.direction = GraphTraversal::Direction::Forward;
  plan.maxDepth = 1U;
  plan.includeStartNodes = false;
  sortAndDedupe(plan.startNodes);
  return plan;
}

GraphTraversal::Plan<std::uint64_t> QueryPlanner::planSymbolDependencyQuery(
    const std::vector<std::uint64_t>& symbolIds) const {
  GraphTraversal::Plan<std::uint64_t> plan;
  plan.startNodes = symbolIds;
  plan.direction = GraphTraversal::Direction::Forward;
  plan.maxDepth = 1U;
  plan.includeStartNodes = false;
  sortAndDedupe(plan.startNodes);
  return plan;
}

GraphTraversal::Plan<std::uint32_t> QueryPlanner::planImpactRegionQuery(
    const std::vector<std::uint32_t>& referenceFileIds,
    const std::size_t maxDepth) const {
  GraphTraversal::Plan<std::uint32_t> plan;
  plan.startNodes = referenceFileIds;
  plan.direction = GraphTraversal::Direction::Reverse;
  plan.maxDepth = maxDepth;
  plan.includeStartNodes = true;
  sortAndDedupe(plan.startNodes);
  return plan;
}

}  // namespace ultra::engine::query

