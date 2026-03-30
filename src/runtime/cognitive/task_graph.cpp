#include "task_graph.h"

#include <algorithm>
#include <utility>

namespace ultra::runtime::cognitive {

const char* toString(const TaskState state) noexcept {
  switch (state) {
    case TaskState::PENDING:
      return "PENDING";
    case TaskState::READY:
      return "READY";
    case TaskState::RUNNING:
      return "RUNNING";
    case TaskState::COMPLETED:
      return "COMPLETED";
    case TaskState::FAILED:
      return "FAILED";
  }
  return "FAILED";
}

bool TaskGraph::add_task(TaskNode node) {
  if (node.id.empty()) {
    return false;
  }
  if (nodes_.find(node.id) != nodes_.end()) {
    return false;
  }

  std::sort(node.dependencies.begin(), node.dependencies.end());
  node.dependencies.erase(
      std::unique(node.dependencies.begin(), node.dependencies.end()),
      node.dependencies.end());

  for (const std::string& dependency : node.dependencies) {
    if (dependency.empty() || dependency == node.id) {
      return false;
    }
    if (nodes_.find(dependency) == nodes_.end()) {
      return false;
    }
  }

  node.state = TaskState::PENDING;
  const std::string taskId = node.id;

  auto inserted = nodes_.emplace(taskId, std::move(node));
  if (!inserted.second) {
    return false;
  }
  outbound_[taskId];
  inbound_[taskId];

  for (const std::string& dependency : inserted.first->second.dependencies) {
    if (would_introduce_cycle(dependency, taskId)) {
      outbound_.erase(taskId);
      inbound_.erase(taskId);
      nodes_.erase(taskId);
      return false;
    }
    outbound_[dependency].insert(taskId);
    inbound_[taskId].insert(dependency);
  }

  refresh_ready_states();
  return true;
}

bool TaskGraph::add_dependency(const std::string& from, const std::string& to) {
  if (from.empty() || to.empty() || from == to) {
    return false;
  }

  auto fromNodeIt = nodes_.find(from);
  auto toNodeIt = nodes_.find(to);
  if (fromNodeIt == nodes_.end() || toNodeIt == nodes_.end()) {
    return false;
  }

  auto& inboundSet = inbound_[to];
  if (inboundSet.find(from) != inboundSet.end()) {
    return true;
  }

  if (would_introduce_cycle(from, to)) {
    return false;
  }

  inboundSet.insert(from);
  outbound_[from].insert(to);

  auto& dependencies = toNodeIt->second.dependencies;
  dependencies.push_back(from);
  std::sort(dependencies.begin(), dependencies.end());
  dependencies.erase(
      std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

  refresh_ready_states();
  return true;
}

std::vector<TaskNode> TaskGraph::get_ready_tasks() {
  refresh_ready_states();

  std::vector<TaskNode> ready;
  for (const auto& [taskId, node] : nodes_) {
    (void)taskId;
    if (node.state == TaskState::READY) {
      ready.push_back(node);
    }
  }

  return ready;
}

bool TaskGraph::mark_running(const std::string& task_id) {
  auto it = nodes_.find(task_id);
  if (it == nodes_.end() || it->second.state != TaskState::READY) {
    return false;
  }

  it->second.state = TaskState::RUNNING;
  return true;
}

bool TaskGraph::mark_completed(const std::string& task_id) {
  auto it = nodes_.find(task_id);
  if (it == nodes_.end()) {
    return false;
  }
  if (it->second.state != TaskState::RUNNING &&
      it->second.state != TaskState::READY) {
    return false;
  }

  it->second.state = TaskState::COMPLETED;
  refresh_ready_states();
  return true;
}

bool TaskGraph::mark_failed(const std::string& task_id) {
  auto it = nodes_.find(task_id);
  if (it == nodes_.end()) {
    return false;
  }
  if (it->second.state != TaskState::RUNNING &&
      it->second.state != TaskState::READY) {
    return false;
  }

  it->second.state = TaskState::FAILED;
  refresh_ready_states();
  return true;
}

bool TaskGraph::reset_failed(const std::string& task_id) {
  auto it = nodes_.find(task_id);
  if (it == nodes_.end() || it->second.state != TaskState::FAILED) {
    return false;
  }

  it->second.state = TaskState::PENDING;
  refresh_ready_states();
  return true;
}

bool TaskGraph::reopen_task(const std::string& task_id) {
  auto it = nodes_.find(task_id);
  if (it == nodes_.end()) {
    return false;
  }

  std::set<std::string> toReopen;
  std::vector<std::string> frontier{task_id};
  while (!frontier.empty()) {
    const std::string current = frontier.back();
    frontier.pop_back();

    if (!toReopen.insert(current).second) {
      continue;
    }

    const auto outboundIt = outbound_.find(current);
    if (outboundIt == outbound_.end()) {
      continue;
    }

    for (const std::string& dependent : outboundIt->second) {
      frontier.push_back(dependent);
    }
  }

  for (const std::string& reopenId : toReopen) {
    auto nodeIt = nodes_.find(reopenId);
    if (nodeIt == nodes_.end()) {
      continue;
    }

    if (nodeIt->second.state != TaskState::COMPLETED &&
        nodeIt->second.state != TaskState::FAILED &&
        nodeIt->second.state != TaskState::READY &&
        nodeIt->second.state != TaskState::RUNNING) {
      continue;
    }

    nodeIt->second.state = TaskState::PENDING;
  }

  refresh_ready_states();
  return true;
}

bool TaskGraph::has_pending_tasks() const {
  for (const auto& [taskId, node] : nodes_) {
    (void)taskId;
    if (node.state != TaskState::COMPLETED) {
      return true;
    }
  }

  return false;
}

std::vector<std::string> TaskGraph::failed_tasks() const {
  std::vector<std::string> failed;
  for (const auto& [taskId, node] : nodes_) {
    if (node.state == TaskState::FAILED) {
      failed.push_back(taskId);
    }
  }

  return failed;
}

bool TaskGraph::empty() const noexcept {
  return nodes_.empty();
}

std::size_t TaskGraph::size() const noexcept {
  return nodes_.size();
}

bool TaskGraph::would_introduce_cycle(const std::string& from,
                                      const std::string& to) const {
  if (from == to) {
    return true;
  }

  return has_path(to, from);
}

bool TaskGraph::has_path(const std::string& from, const std::string& to) const {
  auto fromIt = nodes_.find(from);
  auto toIt = nodes_.find(to);
  if (fromIt == nodes_.end() || toIt == nodes_.end()) {
    return false;
  }

  if (from == to) {
    return true;
  }

  std::set<std::string> visited;
  std::vector<std::string> frontier{from};

  while (!frontier.empty()) {
    const std::string current = frontier.back();
    frontier.pop_back();

    if (!visited.insert(current).second) {
      continue;
    }

    const auto adjacencyIt = outbound_.find(current);
    if (adjacencyIt == outbound_.end()) {
      continue;
    }

    for (const std::string& next : adjacencyIt->second) {
      if (next == to) {
        return true;
      }
      frontier.push_back(next);
    }
  }

  return false;
}

void TaskGraph::refresh_ready_states() {
  for (auto& [taskId, node] : nodes_) {
    (void)taskId;

    if (node.state == TaskState::COMPLETED ||
        node.state == TaskState::FAILED ||
        node.state == TaskState::RUNNING) {
      continue;
    }

    bool allDependenciesCompleted = true;
    for (const std::string& dependency : node.dependencies) {
      const auto dependencyIt = nodes_.find(dependency);
      if (dependencyIt == nodes_.end() ||
          dependencyIt->second.state != TaskState::COMPLETED) {
        allDependenciesCompleted = false;
        break;
      }
    }

    node.state =
        allDependenciesCompleted ? TaskState::READY : TaskState::PENDING;
  }
}

}  // namespace ultra::runtime::cognitive

