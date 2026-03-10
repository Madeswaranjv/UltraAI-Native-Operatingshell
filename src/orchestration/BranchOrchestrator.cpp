// BranchOrchestrator.cpp
#include "BranchOrchestrator.h"
#include "../core/Logger.h"
#include <algorithm>

namespace ultra::orchestration {

BranchOrchestrator::BranchOrchestrator(
    ultra::intelligence::BranchLifecycle& lifecycle)
    : lifecycle_(lifecycle) {}

// ------------------------------------------------------------
// Graph Validation
// ------------------------------------------------------------
bool BranchOrchestrator::validateGraph(const TaskGraph& graph) const {
    auto allNodes = graph.getAllNodes();

    // Empty graph is valid
    if (allNodes.empty()) {
        return true;
    }

    // Rely on TaskGraph's topologicalOrder() to detect cycles
    auto order = graph.topologicalOrder();

    if (order.empty() && !allNodes.empty()) {
        ultra::core::Logger::error(
            ultra::core::LogCategory::General,
            "DAG validation failed: cycle detected in task graph");
        return false;
    }

    return true;
}

// ------------------------------------------------------------
// Deterministic Topological Order
// ------------------------------------------------------------
std::vector<std::string>
BranchOrchestrator::getDeterministicTopologicalOrder(
    const TaskGraph& graph) const {

    auto order = graph.topologicalOrder();

    // If empty graph or cycle (handled by caller), return as-is
    if (order.empty()) {
        return order;
    }

    // Enforce deterministic ordering
    std::sort(order.begin(), order.end());

    return order;
}

// ------------------------------------------------------------
// Spawn Branches
// ------------------------------------------------------------
std::vector<std::string>
BranchOrchestrator::spawnBranches(
    const TaskGraph& graph,
    const std::vector<std::string>& order,
    const std::string& parentBranchId) {

    std::vector<std::string> spawnedBranches;
    spawnedBranches.reserve(order.size());

    for (const auto& taskId : order) {

        SubTask task = graph.getNode(taskId);

        if (task.taskId.empty()) {
            ultra::core::Logger::warning(
                ultra::core::LogCategory::General,
                "Task not found in graph: " + taskId + ", skipping");
            continue;
        }

        ultra::core::Logger::info(
            ultra::core::LogCategory::General,
            "Orchestrating sub-task: " + task.taskId +
            " -> " + task.description);

        ultra::intelligence::Branch spawnedBranch =
            lifecycle_.spawn(parentBranchId, task.description);

        if (!spawnedBranch.branchId.empty()) {
            spawnedBranches.push_back(spawnedBranch.branchId);
        } else {
            ultra::core::Logger::warning(
                ultra::core::LogCategory::General,
                "Failed to spawn branch for task: " + taskId);
        }
    }

    return spawnedBranches;
}

// ------------------------------------------------------------
// Orchestration Entry Point
// ------------------------------------------------------------
std::vector<std::string>
BranchOrchestrator::orchestrate(
    const TaskGraph& graph,
    const std::string& parentBranchId) {

    // Phase 1: Validate
    if (!validateGraph(graph)) {
        ultra::core::Logger::error(
            ultra::core::LogCategory::General,
            "Task graph validation failed - aborting orchestration");
        return {};
    }

    auto allNodes = graph.getAllNodes();
    if (allNodes.empty()) {
        return {};
    }

    // Phase 2: Deterministic ordering
    auto order = getDeterministicTopologicalOrder(graph);
    if (order.empty()) {
        ultra::core::Logger::error(
            ultra::core::LogCategory::General,
            "Failed to compute topological order");
        return {};
    }

    // Phase 3: Spawn
    return spawnBranches(graph, order, parentBranchId);
}

}  // namespace ultra::orchestration