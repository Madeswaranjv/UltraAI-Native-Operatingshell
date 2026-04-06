#include "task_graph.h"

#include "contract_enforcement.h"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <utility>

namespace ultra::runtime::cognitive {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hashByte(std::uint64_t& hash, const unsigned char value) noexcept {
  hash ^= static_cast<std::uint64_t>(value);
  hash *= kFnvPrime;
}

void hashString(std::uint64_t& hash, const std::string_view value) noexcept {
  for (const unsigned char ch : value) {
    hashByte(hash, ch);
  }
  hashByte(hash, 0xFFU);
}

void hashUint64(std::uint64_t& hash, const std::uint64_t value) noexcept {
  for (std::size_t index = 0U; index < sizeof(value); ++index) {
    const unsigned char byte = static_cast<unsigned char>(
        (value >> (index * 8U)) & 0xFFU);
    hashByte(hash, byte);
  }
}

std::uint64_t computeStateHash(
    const std::map<std::string, TaskNode>& nodes) noexcept {
  std::uint64_t hash = kFnvOffsetBasis;

  for (const auto& [taskId, node] : nodes) {
    hashString(hash, "task");
    hashString(hash, taskId);
    hashUint64(hash, static_cast<std::uint64_t>(node.state));
    for (const std::string& dependency : node.dependencies) {
      hashString(hash, "dep");
      hashString(hash, dependency);
    }
  }

  return hash;
}

std::string buildRepairOverlayId(const std::uint64_t baseStateHash,
                                 const std::set<std::string>& affectedNodes,
                                 const std::size_t attempt) {
  std::uint64_t hash = kFnvOffsetBasis;
  hashUint64(hash, baseStateHash);
  hashUint64(hash, static_cast<std::uint64_t>(attempt));
  for (const std::string& taskId : affectedNodes) {
    hashString(hash, taskId);
  }

  std::ostringstream stream;
  stream << "repair_overlay_" << std::hex << hash;
  return stream.str();
}

void assertRepairPhaseOverlayIsolation(const std::string_view location) {
  if (::ultra::runtime::contracts::ContractValidator::currentPhase() !=
      ::ultra::runtime::contracts::LoopPhase::PARTIAL_REPAIR) {
    return;
  }

  throw ::ultra::runtime::contracts::ContractViolationException({
      ::ultra::runtime::contracts::LayerId::L5_OVERLAY,
      ::ultra::runtime::contracts::ViolationType::OverlayBypass,
      std::string(location),
      "Partial repair must mutate an isolated repair overlay, not the base "
      "TaskGraph.",
      ::ultra::runtime::contracts::ContractValidator::currentPhase(),
  });
}

template <typename ResolveState>
void refreshReadyStatesForNodes(std::map<std::string, TaskNode>& nodes,
                                ResolveState&& resolveState) {
  for (auto& [taskId, node] : nodes) {
    (void)taskId;

    if (node.state == TaskState::COMPLETED ||
        node.state == TaskState::FAILED ||
        node.state == TaskState::RUNNING) {
      continue;
    }

    bool allDependenciesCompleted = true;
    for (const std::string& dependency : node.dependencies) {
      if (resolveState(dependency) != TaskState::COMPLETED) {
        allDependenciesCompleted = false;
        break;
      }
    }

    node.state =
        allDependenciesCompleted ? TaskState::READY : TaskState::PENDING;
  }
}

}  // namespace

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

bool TaskGraphRepairOverlay::reopen_task(const std::string& task_id) {
  if (!contains_task(task_id) || parentSnapshot_ == nullptr) {
    return false;
  }

  std::set<std::string> toReopen;
  std::vector<std::string> frontier{task_id};
  while (!frontier.empty()) {
    const std::string current = frontier.back();
    frontier.pop_back();

    if (!contains_task(current) || !toReopen.insert(current).second) {
      continue;
    }

    const auto outboundIt = parentSnapshot_->outbound.find(current);
    if (outboundIt == parentSnapshot_->outbound.end()) {
      continue;
    }

    for (const std::string& dependent : outboundIt->second) {
      frontier.push_back(dependent);
    }
  }

  bool reopened = false;
  for (const std::string& reopenId : toReopen) {
    auto nodeIt = overlayNodes_.find(reopenId);
    if (nodeIt == overlayNodes_.end()) {
      continue;
    }

    if (nodeIt->second.state != TaskState::COMPLETED &&
        nodeIt->second.state != TaskState::FAILED &&
        nodeIt->second.state != TaskState::READY &&
        nodeIt->second.state != TaskState::RUNNING) {
      continue;
    }

    nodeIt->second.state = TaskState::PENDING;
    reopened = true;
  }

  if (reopened) {
    refresh_ready_states();
  }
  return reopened;
}

bool TaskGraphRepairOverlay::reset_failed(const std::string& task_id) {
  if (!contains_task(task_id) || resolve_state(task_id) != TaskState::FAILED) {
    return false;
  }

  overlayNodes_[task_id].state = TaskState::PENDING;
  refresh_ready_states();
  return true;
}

bool TaskGraphRepairOverlay::mark_completed(const std::string& task_id) {
  if (!contains_task(task_id)) {
    return false;
  }

  const TaskState currentState = resolve_state(task_id);
  if (currentState != TaskState::RUNNING && currentState != TaskState::READY) {
    return false;
  }

  overlayNodes_[task_id].state = TaskState::COMPLETED;
  refresh_ready_states();
  return true;
}

bool TaskGraphRepairOverlay::empty() const noexcept {
  return parentSnapshot_ == nullptr || overlayNodes_.empty() ||
         affectedNodeIds_.empty();
}

bool TaskGraphRepairOverlay::verify() {
  metadata_.verified = false;
  if (empty() || overlayNodes_.size() != affectedNodeIds_.size() ||
      metadata_.affectedNodes.size() != affectedNodeIds_.size()) {
    return false;
  }

  for (const auto& [taskId, node] : overlayNodes_) {
    if (!contains_task(taskId)) {
      return false;
    }

    const auto parentIt = parentSnapshot_->nodes.find(taskId);
    if (parentIt == parentSnapshot_->nodes.end()) {
      return false;
    }
    if (node.id != parentIt->second.id ||
        node.dependencies != parentIt->second.dependencies) {
      return false;
    }
  }

  for (const std::string& taskId : metadata_.affectedNodes) {
    if (!contains_task(taskId)) {
      return false;
    }
  }

  metadata_.verified = true;
  return true;
}

const TaskGraphRepairMetadata& TaskGraphRepairOverlay::metadata() const noexcept {
  return metadata_;
}

bool TaskGraphRepairOverlay::contains_task(
    const std::string& task_id) const noexcept {
  return affectedNodeIds_.find(task_id) != affectedNodeIds_.end();
}

TaskState TaskGraphRepairOverlay::resolve_state(const std::string& task_id) const {
  const auto overlayIt = overlayNodes_.find(task_id);
  if (overlayIt != overlayNodes_.end()) {
    return overlayIt->second.state;
  }

  if (parentSnapshot_ == nullptr) {
    return TaskState::FAILED;
  }

  const auto parentIt = parentSnapshot_->nodes.find(task_id);
  return parentIt == parentSnapshot_->nodes.end() ? TaskState::FAILED
                                                  : parentIt->second.state;
}

void TaskGraphRepairOverlay::refresh_ready_states() {
  refreshReadyStatesForNodes(
      overlayNodes_, [this](const std::string& dependency) {
        return resolve_state(dependency);
      });
}

bool TaskGraph::add_task(TaskNode node) {
  assertRepairPhaseOverlayIsolation("TaskGraph::add_task");
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
  assertRepairPhaseOverlayIsolation("TaskGraph::add_dependency");
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
  assertRepairPhaseOverlayIsolation("TaskGraph::mark_running");
  auto it = nodes_.find(task_id);
  if (it == nodes_.end() || it->second.state != TaskState::READY) {
    return false;
  }

  it->second.state = TaskState::RUNNING;
  return true;
}

bool TaskGraph::mark_completed(const std::string& task_id) {
  assertRepairPhaseOverlayIsolation("TaskGraph::mark_completed");
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
  assertRepairPhaseOverlayIsolation("TaskGraph::mark_failed");
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
  assertRepairPhaseOverlayIsolation("TaskGraph::reset_failed");
  auto it = nodes_.find(task_id);
  if (it == nodes_.end() || it->second.state != TaskState::FAILED) {
    return false;
  }

  it->second.state = TaskState::PENDING;
  refresh_ready_states();
  return true;
}

bool TaskGraph::reopen_task(const std::string& task_id) {
  assertRepairPhaseOverlayIsolation("TaskGraph::reopen_task");
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

TaskGraphRepairOverlay TaskGraph::create_repair_overlay(
    const std::vector<std::string>& task_ids,
    const std::size_t attempt) const {
  TaskGraphRepairOverlay overlay;

  const std::set<std::string> affectedNodeIds = collect_repair_subgraph(task_ids);
  if (affectedNodeIds.empty()) {
    return overlay;
  }

  TaskGraphSnapshot snapshot;
  snapshot.nodes = nodes_;
  snapshot.outbound = outbound_;
  snapshot.inbound = inbound_;
  snapshot.structuralHash = structural_hash();
  snapshot.stateHash = state_hash();

  overlay.parentSnapshot_ =
      std::make_shared<const TaskGraphSnapshot>(std::move(snapshot));
  overlay.affectedNodeIds_ = affectedNodeIds;
  overlay.metadata_.overlayId = buildRepairOverlayId(
      overlay.parentSnapshot_->stateHash, affectedNodeIds, attempt);
  overlay.metadata_.affectedNodes.assign(affectedNodeIds.begin(),
                                         affectedNodeIds.end());

  for (const std::string& taskId : affectedNodeIds) {
    const auto nodeIt = nodes_.find(taskId);
    if (nodeIt != nodes_.end()) {
      overlay.overlayNodes_.emplace(taskId, nodeIt->second);
    }
  }

  return overlay;
}

bool TaskGraph::verify_repair_overlay(
    const TaskGraphRepairOverlay& overlay) const {
  ::ultra::runtime::contracts::ContractValidator::assertInvariant(
      overlay.parentSnapshot_ != nullptr,
      ::ultra::runtime::contracts::LayerId::L5_OVERLAY,
      ::ultra::runtime::contracts::ViolationType::OverlayBypass,
      "TaskGraph::verify_repair_overlay",
      "Repair overlay is missing a parent snapshot.");

  if (!overlay.metadata_.verified) {
    throw ::ultra::runtime::contracts::ContractViolationException({
        ::ultra::runtime::contracts::LayerId::L5_OVERLAY,
        ::ultra::runtime::contracts::ViolationType::OverlayBypass,
        "TaskGraph::verify_repair_overlay",
        "Repair overlay merge requires prior verification.",
        ::ultra::runtime::contracts::ContractValidator::currentPhase(),
    });
  }

  if (overlay.parentSnapshot_->structuralHash != structural_hash() ||
      overlay.parentSnapshot_->stateHash != state_hash()) {
    throw ::ultra::runtime::contracts::ContractViolationException({
        ::ultra::runtime::contracts::LayerId::L4_GRAPH,
        ::ultra::runtime::contracts::ViolationType::ImmutableMutation,
        "TaskGraph::verify_repair_overlay",
        "Repair base snapshot changed before merge.",
        ::ultra::runtime::contracts::ContractValidator::currentPhase(),
    });
  }

  return !overlay.empty();
}

std::optional<TaskGraph> TaskGraph::merge_repair_overlay(
    const TaskGraphRepairOverlay& overlay) const {
  if (!verify_repair_overlay(overlay)) {
    return std::nullopt;
  }

  TaskGraph merged = *this;
  for (const auto& [taskId, node] : overlay.overlayNodes_) {
    auto nodeIt = merged.nodes_.find(taskId);
    if (nodeIt == merged.nodes_.end()) {
      throw ::ultra::runtime::contracts::ContractViolationException({
          ::ultra::runtime::contracts::LayerId::L5_OVERLAY,
          ::ultra::runtime::contracts::ViolationType::InvariantFailure,
          "TaskGraph::merge_repair_overlay",
          "Repair overlay references an unknown task: " + taskId,
          ::ultra::runtime::contracts::ContractValidator::currentPhase(),
      });
    }
    nodeIt->second.state = node.state;
  }
  merged.refresh_ready_states();
  return merged;
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

std::set<std::string> TaskGraph::collect_repair_subgraph(
    const std::vector<std::string>& task_ids) const {
  std::set<std::string> affectedNodeIds;
  std::vector<std::string> frontier(task_ids.begin(), task_ids.end());

  while (!frontier.empty()) {
    const std::string current = frontier.back();
    frontier.pop_back();

    if (nodes_.find(current) == nodes_.end() ||
        !affectedNodeIds.insert(current).second) {
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

  return affectedNodeIds;
}

std::uint64_t TaskGraph::structural_hash() const noexcept {
  std::uint64_t hash = kFnvOffsetBasis;

  for (const auto& [taskId, node] : nodes_) {
    (void)node;
    hashString(hash, "task");
    hashString(hash, taskId);
    for (const std::string& dependency : node.dependencies) {
      hashString(hash, "dep");
      hashString(hash, dependency);
    }
  }

  return hash;
}

std::uint64_t TaskGraph::state_hash() const noexcept {
  return computeStateHash(nodes_);
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
  refreshReadyStatesForNodes(
      nodes_, [this](const std::string& dependency) {
        const auto dependencyIt = nodes_.find(dependency);
        return dependencyIt == nodes_.end() ? TaskState::FAILED
                                            : dependencyIt->second.state;
      });
}

}  // namespace ultra::runtime::cognitive
