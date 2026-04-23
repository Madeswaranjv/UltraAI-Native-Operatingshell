#pragma once

#include "ExecutionKernel.h"
#include "../intent/Strategy.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ultra::runtime::cognitive {

enum class TaskState : std::uint8_t {
  PENDING = 0U,
  READY = 1U,
  RUNNING = 2U,
  COMPLETED = 3U,
  FAILED = 4U
};

[[nodiscard]] const char* toString(TaskState state) noexcept;

enum class TaskPayloadKind : std::uint8_t {
  Action = 0U,
  Intent = 1U
};

struct TaskPayload {
  TaskPayloadKind kind{TaskPayloadKind::Action};
  ::ultra::runtime::Action action{};
  ::ultra::runtime::intent::Intent intent{};
  ::ultra::runtime::governance::Policy policy{};
  std::optional<::ultra::runtime::intent::Action> plannedAction{};
};

struct TaskNode {
  std::string id;
  std::vector<std::string> dependencies;
  TaskState state{TaskState::PENDING};
  TaskPayload payload{};
};

struct TaskGraphSnapshot {
  std::map<std::string, TaskNode> nodes;
  std::map<std::string, std::set<std::string>> outbound;
  std::map<std::string, std::set<std::string>> inbound;
  std::uint64_t structuralHash{0U};
  std::uint64_t stateHash{0U};
};

struct TaskGraphRepairMetadata {
  std::string overlayId;
  std::vector<std::string> affectedNodes;
  std::vector<std::string> repairNodes;
  bool verified{false};
};

class TaskGraphRepairOverlay {
 public:
  bool reopen_task(const std::string& task_id);
  bool reset_failed(const std::string& task_id);
  bool mark_completed(const std::string& task_id);

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] bool verify();
  [[nodiscard]] const TaskGraphRepairMetadata& metadata() const noexcept;

 private:
  friend class TaskGraph;

  [[nodiscard]] bool contains_task(const std::string& task_id) const noexcept;
  [[nodiscard]] TaskState resolve_state(const std::string& task_id) const;
  void refresh_ready_states();

  std::shared_ptr<const TaskGraphSnapshot> parentSnapshot_;
  std::map<std::string, TaskNode> overlayNodes_;
  std::set<std::string> affectedNodeIds_;
  TaskGraphRepairMetadata metadata_{};
};

class TaskGraph {
 public:
  bool add_task(TaskNode node);
  bool add_dependency(const std::string& from, const std::string& to);

  std::vector<TaskNode> get_ready_tasks();

  bool mark_running(const std::string& task_id);
  bool mark_completed(const std::string& task_id);
  bool mark_failed(const std::string& task_id);
  bool reset_failed(const std::string& task_id);
  bool reopen_task(const std::string& task_id);

  [[nodiscard]] TaskGraphRepairOverlay create_repair_overlay(
      const std::vector<std::string>& task_ids,
      std::size_t attempt = 0U) const;
  [[nodiscard]] bool verify_repair_overlay(
      const TaskGraphRepairOverlay& overlay) const;
  [[nodiscard]] std::optional<TaskGraph> merge_repair_overlay(
      const TaskGraphRepairOverlay& overlay) const;

  [[nodiscard]] bool has_pending_tasks() const;
  [[nodiscard]] std::vector<std::string> failed_tasks() const;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t structural_hash() const noexcept;

 private:
  [[nodiscard]] std::set<std::string> collect_repair_subgraph(
      const std::vector<std::string>& task_ids) const;
  [[nodiscard]] std::uint64_t state_hash() const noexcept;
  [[nodiscard]] bool would_introduce_cycle(const std::string& from,
                                           const std::string& to) const;
  [[nodiscard]] bool has_path(const std::string& from,
                              const std::string& to) const;
  void rebuild_adjacency();
  void refresh_ready_states();

  std::map<std::string, TaskNode> nodes_;
  std::map<std::string, std::set<std::string>> outbound_;
  std::map<std::string, std::set<std::string>> inbound_;
};

}  // namespace ultra::runtime::cognitive
