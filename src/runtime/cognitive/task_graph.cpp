#include "task_graph.h"

#include "contract_enforcement.h"

#include <algorithm>
#include <iostream>
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

std::string truncateText(std::string value, const std::size_t limit) {
  if (value.size() <= limit) {
    return value;
  }

  constexpr std::string_view kNotice = "\n...[truncated]";
  const std::size_t keep = limit > kNotice.size() ? limit - kNotice.size() : 0U;
  value.resize(keep);
  value += kNotice;
  return value;
}

bool endsWithRepairSuffix(const std::string_view value) {
  constexpr std::string_view kSuffix = "_repair";
  return value.size() >= kSuffix.size() &&
         value.substr(value.size() - kSuffix.size()) == kSuffix;
}

std::string repairTaskIdFor(const std::string& taskId) {
  if (taskId.empty() || endsWithRepairSuffix(taskId)) {
    return taskId;
  }
  return taskId + "_repair";
}

std::vector<std::string> uniqueDependencies(std::vector<std::string> dependencies) {
  dependencies.erase(
      std::remove_if(dependencies.begin(),
                     dependencies.end(),
                     [](const std::string& value) { return value.empty(); }),
      dependencies.end());
  std::sort(dependencies.begin(), dependencies.end());
  dependencies.erase(
      std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
  return dependencies;
}

std::string actionTypeLabel(const ::ultra::runtime::ActionType type) {
  switch (type) {
    case ::ultra::runtime::ActionType::Mutation:
      return "Mutation";
    case ::ultra::runtime::ActionType::ImpactPrediction:
      return "ImpactPrediction";
    case ::ultra::runtime::ActionType::ContextExtraction:
      return "ContextExtraction";
    case ::ultra::runtime::ActionType::BranchDiff:
      return "BranchDiff";
    case ::ultra::runtime::ActionType::SimulateChange:
      return "SimulateChange";
    case ::ultra::runtime::ActionType::IntentEvaluation:
      return "IntentEvaluation";
    case ::ultra::runtime::ActionType::ModelGenerate:
      return "ModelGenerate";
    case ::ultra::runtime::ActionType::ToolExecution:
      return "ToolExecution";
  }
  return "Mutation";
}

std::string payloadTarget(const TaskNode& node) {
  if (node.payload.kind == TaskPayloadKind::Action) {
    if (node.payload.plannedAction.has_value() &&
        !node.payload.plannedAction->target.empty()) {
      return node.payload.plannedAction->target;
    }
    if (!node.payload.action.target.empty()) {
      return node.payload.action.target;
    }
  } else if (!node.payload.intent.goal.target.empty()) {
    return node.payload.intent.goal.target;
  }
  return node.id;
}

std::string payloadSourceFile(const TaskNode& node) {
  if (node.payload.kind != TaskPayloadKind::Action ||
      !node.payload.action.modelRequest.has_value() ||
      !node.payload.action.modelRequest->contextPayload.is_object()) {
    return {};
  }

  const auto& payload = node.payload.action.modelRequest->contextPayload;
  if (payload.contains("source_file") && payload.at("source_file").is_string()) {
    return payload.at("source_file").get<std::string>();
  }
  if (payload.contains("file") && payload.at("file").is_string()) {
    return payload.at("file").get<std::string>();
  }
  return {};
}

std::string originalTaskIdForNode(const TaskNode& node) {
  if (node.payload.kind == TaskPayloadKind::Action &&
      node.payload.action.modelRequest.has_value() &&
      node.payload.action.modelRequest->contextPayload.is_object() &&
      node.payload.action.modelRequest->contextPayload.contains("original_task") &&
      node.payload.action.modelRequest->contextPayload.at("original_task").is_string()) {
    return node.payload.action.modelRequest->contextPayload.at("original_task")
        .get<std::string>();
  }
  return endsWithRepairSuffix(node.id) ? node.id : node.id;
}

std::string summarizePayload(const TaskNode& node) {
  std::ostringstream stream;
  stream << "Task ID: " << node.id << "\n";
  if (node.payload.kind == TaskPayloadKind::Action) {
    stream << "Payload Kind: Action\n";
    stream << "Action Type: " << actionTypeLabel(node.payload.action.type) << "\n";
    stream << "Target: " << payloadTarget(node) << "\n";
    if (node.payload.plannedAction.has_value()) {
      stream << "Planned Action: "
             << ::ultra::runtime::intent::toString(node.payload.plannedAction->kind)
             << "\n";
      if (!node.payload.plannedAction->details.empty()) {
        stream << "Planned Details: "
               << truncateText(node.payload.plannedAction->details, 240U) << "\n";
      }
    }
    if (!node.payload.action.toolName.empty()) {
      stream << "Tool: " << node.payload.action.toolName << "\n";
    }
    if (node.payload.action.modelRequest.has_value()) {
      stream << "Original Prompt:\n"
             << truncateText(node.payload.action.modelRequest->prompt, 900U) << "\n";
    }
  } else {
    stream << "Payload Kind: Intent\n";
    stream << "Intent Target: " << node.payload.intent.goal.target << "\n";
    stream << "Intent Goal: "
           << ::ultra::runtime::intent::toString(node.payload.intent.goal.type)
           << "\n";
  }
  return truncateText(stream.str(), 1400U);
}

::ultra::ai::orchestration::TaskType taskTypeForRepairRole(
    const std::string& role) {
  if (role == "coder") {
    return ::ultra::ai::orchestration::TaskType::Coding;
  }
  if (role == "planner") {
    return ::ultra::ai::orchestration::TaskType::Planning;
  }
  return ::ultra::ai::orchestration::TaskType::Analysis;
}

std::string toolRequirementForFailure(const ::ultra::runtime::FailureType type) {
  if (type == ::ultra::runtime::FailureType::TEST_FAIL) {
    return "run_command";
  }
  if (type == ::ultra::runtime::FailureType::NONE) {
    return {};
  }
  return "apply_patch";
}

::ultra::ai::model::ModelRequest buildRepairModelRequest(
    const TaskNode& originalNode,
    const ::ultra::runtime::FailureIntelligence& intelligence) {
  ::ultra::ai::model::ModelRequest request;
  request.systemPrompt =
      "You are UltraInfinity deterministic repair execution. Use only real "
      "workspace tools. Classify the failure evidence, repair the targeted issue, "
      "and return tool calls only. Do not answer with prose.";

  std::ostringstream prompt;
  prompt << "Repair Task: " << intelligence.repairTaskId << "\n";
  prompt << "Original Task: " << intelligence.originalTaskId << "\n";
  prompt << "Failure Type: " << ::ultra::runtime::toString(intelligence.type)
         << "\n";
  prompt << "Requested Repair Role: " << intelligence.routedRole << "\n";
  prompt << "Target: " << intelligence.target << "\n";
  if (!intelligence.sourceFile.empty()) {
    prompt << "Source File: " << intelligence.sourceFile << "\n";
  }
  prompt << "Original Task Summary:\n" << summarizePayload(originalNode) << "\n";
  if (!intelligence.previousOutput.empty()) {
    prompt << "\nPrevious LLM Output:\n"
           << truncateText(intelligence.previousOutput, 1200U) << "\n";
  }
  if (!intelligence.errorLogs.empty()) {
    prompt << "\nFailure Logs:\n"
           << truncateText(intelligence.errorLogs, 1800U) << "\n";
  }
  prompt << "\nApply the smallest targeted repair needed so the original task and "
            "its dependent tasks can be re-executed.";
  request.prompt = truncateText(prompt.str(), 4096U);
  request.temperature = 0.0;
  request.maxTokens = 1024U;
  request.toolsAvailable = {"apply_patch", "query_symbol", "read_source",
                            "impact_analysis", "search_files", "run_command"};
  request.contextPayload = {
      {"failure_type", ::ultra::runtime::toString(intelligence.type)},
      {"model_role", intelligence.routedRole},
      {"original_task", intelligence.originalTaskId},
      {"repair_context", intelligence.repairContext},
      {"repair_task", true},
      {"requested_role", intelligence.routedRole},
      {"stage", intelligence.routedRole},
      {"target", intelligence.target},
      {"task_id", intelligence.repairTaskId},
      {"type", "repair"},
  };

  const std::string toolRequired = toolRequirementForFailure(intelligence.type);
  if (!toolRequired.empty()) {
    request.contextPayload["tool_required"] = toolRequired;
  }
  if (!intelligence.sourceFile.empty()) {
    request.contextPayload["source_file"] = intelligence.sourceFile;
  }
  return request;
}

::ultra::ai::orchestration::OrchestrationContext buildRepairOrchestrationContext(
    const ::ultra::runtime::FailureIntelligence& intelligence) {
  ::ultra::ai::orchestration::OrchestrationContext context;
  context.taskType = taskTypeForRepairRole(intelligence.routedRole);
  context.complexity =
      intelligence.type == ::ultra::runtime::FailureType::LOGIC_ERROR
          ? ::ultra::ai::orchestration::TaskComplexity::Medium
          : ::ultra::ai::orchestration::TaskComplexity::High;
  context.priority = ::ultra::ai::orchestration::TaskPriority::Urgent;
  context.tokenBudget = 1024U;
  context.modelRoleHint = intelligence.routedRole;
  return context;
}

TaskNode buildRepairNode(const TaskNode& originalNode,
                         const ::ultra::runtime::FailureIntelligence& intelligence,
                         std::vector<std::string> dependencies) {
  TaskNode repairNode;
  repairNode.id = intelligence.repairTaskId;
  repairNode.dependencies = uniqueDependencies(std::move(dependencies));
  repairNode.state = TaskState::PENDING;
  repairNode.payload.kind = TaskPayloadKind::Action;
  repairNode.payload.action.id = intelligence.repairTaskId;
  repairNode.payload.action.type = ::ultra::runtime::ActionType::ModelGenerate;
  repairNode.payload.action.target = intelligence.target.empty()
                                         ? payloadTarget(originalNode)
                                         : intelligence.target;
  repairNode.payload.action.branch = originalNode.payload.action.branch;
  repairNode.payload.action.snapshotVersion =
      originalNode.payload.action.snapshotVersion;
  repairNode.payload.action.modelRequest =
      buildRepairModelRequest(originalNode, intelligence);
  repairNode.payload.action.orchestrationContext =
      buildRepairOrchestrationContext(intelligence);
  repairNode.payload.action.modelProvider = originalNode.payload.action.modelProvider;
  return repairNode;
}

::ultra::runtime::FailureIntelligence fallbackVerificationIntelligence(
    const TaskNode& node) {
  ::ultra::runtime::FailureIntelligence intelligence;
  intelligence.type = ::ultra::runtime::FailureType::TEST_FAIL;
  intelligence.taskId = node.id;
  intelligence.originalTaskId = originalTaskIdForNode(node);
  intelligence.repairTaskId = repairTaskIdFor(intelligence.originalTaskId);
  intelligence.target = payloadTarget(node);
  intelligence.sourceFile = payloadSourceFile(node);
  intelligence.routedRole = ::ultra::runtime::ExecutionKernel::routeFailureRole(
      intelligence.type);
  intelligence.errorLogs = "Verification stage requested deterministic retry.";
  intelligence.repairContext = {
      {"context",
       {{"error_logs", intelligence.errorLogs},
        {"previous_output", intelligence.previousOutput}}},
      {"failure_type", ::ultra::runtime::toString(intelligence.type)},
      {"original_task", intelligence.originalTaskId},
      {"target", intelligence.target},
      {"task_id", intelligence.repairTaskId},
      {"type", "repair"},
  };
  if (!intelligence.sourceFile.empty()) {
    intelligence.repairContext["source_file"] = intelligence.sourceFile;
  }
  return intelligence;
}

bool rebuildAdjacencyFromNodes(
    const std::map<std::string, TaskNode>& nodes,
    std::map<std::string, std::set<std::string>>& outbound,
    std::map<std::string, std::set<std::string>>& inbound) {
  outbound.clear();
  inbound.clear();

  for (const auto& [taskId, node] : nodes) {
    outbound[taskId];
    inbound[taskId];
    for (const std::string& dependency : node.dependencies) {
      if (dependency.empty() || dependency == taskId ||
          nodes.find(dependency) == nodes.end()) {
        return false;
      }
      outbound[dependency].insert(taskId);
      inbound[taskId].insert(dependency);
    }
  }

  std::map<std::string, std::size_t> indegree;
  for (const auto& [taskId, deps] : inbound) {
    indegree[taskId] = deps.size();
  }

  std::vector<std::string> ready;
  for (const auto& [taskId, degree] : indegree) {
    if (degree == 0U) {
      ready.push_back(taskId);
    }
  }

  std::size_t visited = 0U;
  while (!ready.empty()) {
    const std::string current = ready.back();
    ready.pop_back();
    ++visited;

    const auto adjacencyIt = outbound.find(current);
    if (adjacencyIt == outbound.end()) {
      continue;
    }

    for (const std::string& dependent : adjacencyIt->second) {
      auto degreeIt = indegree.find(dependent);
      if (degreeIt == indegree.end() || degreeIt->second == 0U) {
        continue;
      }
      --degreeIt->second;
      if (degreeIt->second == 0U) {
        ready.push_back(dependent);
      }
    }
  }

  return visited == nodes.size();
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

  for (const std::string& taskId : metadata_.affectedNodes) {
    if (!contains_task(taskId)) {
      return false;
    }
  }

  for (const auto& [taskId, node] : overlayNodes_) {
    if (!contains_task(taskId) || node.id != taskId) {
      return false;
    }
  }

  std::map<std::string, TaskNode> candidateNodes = parentSnapshot_->nodes;
  for (const auto& [taskId, node] : overlayNodes_) {
    candidateNodes[taskId] = node;
  }

  std::map<std::string, std::set<std::string>> candidateOutbound;
  std::map<std::string, std::set<std::string>> candidateInbound;
  if (!rebuildAdjacencyFromNodes(candidateNodes,
                                 candidateOutbound,
                                 candidateInbound)) {
    return false;
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

  std::set<std::string> affectedNodeIds = collect_repair_subgraph(task_ids);
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

  for (const std::string& taskId : affectedNodeIds) {
    const auto nodeIt = nodes_.find(taskId);
    if (nodeIt != nodes_.end()) {
      overlay.overlayNodes_.emplace(taskId, nodeIt->second);
    }
  }

  for (const std::string& taskId : task_ids) {
    const auto nodeIt = nodes_.find(taskId);
    if (nodeIt == nodes_.end()) {
      continue;
    }

    bool verificationFallback = false;
    std::optional<::ultra::runtime::FailureIntelligence> intelligence =
        ::ultra::runtime::ExecutionKernel::failureIntelligenceForTask(taskId);
    if (!intelligence.has_value() && nodeIt->second.state != TaskState::FAILED) {
      intelligence = fallbackVerificationIntelligence(nodeIt->second);
      verificationFallback = true;
    }
    if (!intelligence.has_value() ||
        intelligence->type == ::ultra::runtime::FailureType::NONE) {
      continue;
    }

    if (intelligence->originalTaskId.empty()) {
      intelligence->originalTaskId = originalTaskIdForNode(nodeIt->second);
    }
    if (intelligence->repairTaskId.empty()) {
      intelligence->repairTaskId = repairTaskIdFor(intelligence->originalTaskId);
    }
    if (intelligence->target.empty()) {
      intelligence->target = payloadTarget(nodeIt->second);
    }
    if (intelligence->sourceFile.empty()) {
      intelligence->sourceFile = payloadSourceFile(nodeIt->second);
    }
    if (verificationFallback) {
      std::cout << "[FAILURE] task=" << intelligence->originalTaskId
                << " type=" << ::ultra::runtime::toString(intelligence->type)
                << std::endl;
      std::cout << "[REPAIR] routing_to=" << intelligence->routedRole
                << std::endl;
    }

    const std::string repairNodeId = intelligence->repairTaskId;
    if (repairNodeId.empty()) {
      continue;
    }

    auto addRepairNodeMetadata = [&](const std::string& repairId) {
      if (std::find(overlay.metadata_.repairNodes.begin(),
                    overlay.metadata_.repairNodes.end(),
                    repairId) == overlay.metadata_.repairNodes.end()) {
        overlay.metadata_.repairNodes.push_back(repairId);
      }
    };

    if (repairNodeId == taskId) {
      auto repairIt = overlay.overlayNodes_.find(taskId);
      if (repairIt == overlay.overlayNodes_.end()) {
        continue;
      }
      TaskNode refreshedRepair = buildRepairNode(
          nodeIt->second, *intelligence, repairIt->second.dependencies);
      refreshedRepair.id = repairIt->second.id;
      refreshedRepair.state = TaskState::PENDING;
      overlay.overlayNodes_[taskId] = std::move(refreshedRepair);
      addRepairNodeMetadata(taskId);
      continue;
    }

    std::vector<std::string> repairDependencies = nodeIt->second.dependencies;
    repairDependencies.erase(
        std::remove(repairDependencies.begin(),
                    repairDependencies.end(),
                    repairNodeId),
        repairDependencies.end());
    repairDependencies = uniqueDependencies(std::move(repairDependencies));

    TaskNode repairNode = buildRepairNode(nodeIt->second,
                                          *intelligence,
                                          repairDependencies);
    repairNode.state = TaskState::PENDING;
    overlay.overlayNodes_[repairNodeId] = std::move(repairNode);
    affectedNodeIds.insert(repairNodeId);
    addRepairNodeMetadata(repairNodeId);

    auto overlayNodeIt = overlay.overlayNodes_.find(taskId);
    if (overlayNodeIt != overlay.overlayNodes_.end()) {
      std::vector<std::string> reopenedDependencies = repairDependencies;
      reopenedDependencies.push_back(repairNodeId);
      overlayNodeIt->second.dependencies =
          uniqueDependencies(std::move(reopenedDependencies));
    }
  }

  overlay.affectedNodeIds_ = affectedNodeIds;
  overlay.metadata_.overlayId = buildRepairOverlayId(
      overlay.parentSnapshot_->stateHash, affectedNodeIds, attempt);
  overlay.metadata_.affectedNodes.assign(affectedNodeIds.begin(),
                                         affectedNodeIds.end());
  overlay.refresh_ready_states();

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
    merged.nodes_[taskId] = node;
  }
  merged.rebuild_adjacency();
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

void TaskGraph::rebuild_adjacency() {
  if (rebuildAdjacencyFromNodes(nodes_, outbound_, inbound_)) {
    return;
  }

  throw ::ultra::runtime::contracts::ContractViolationException({
      ::ultra::runtime::contracts::LayerId::L5_OVERLAY,
      ::ultra::runtime::contracts::ViolationType::InvariantFailure,
      "TaskGraph::rebuild_adjacency",
      "Repair overlay produced an invalid task graph.",
      ::ultra::runtime::contracts::ContractValidator::currentPhase(),
  });
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
