#pragma once

#include "ExecutionKernel.h"

#include <cstddef>
#include <cstdint>
#include <map>
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
};

struct TaskNode {
  std::string id;
  std::vector<std::string> dependencies;
  TaskState state{TaskState::PENDING};
  TaskPayload payload{};
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

  [[nodiscard]] bool has_pending_tasks() const;
  [[nodiscard]] std::vector<std::string> failed_tasks() const;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  [[nodiscard]] bool would_introduce_cycle(const std::string& from,
                                           const std::string& to) const;
  [[nodiscard]] bool has_path(const std::string& from,
                              const std::string& to) const;
  void refresh_ready_states();

  std::map<std::string, TaskNode> nodes_;
  std::map<std::string, std::set<std::string>> outbound_;
  std::map<std::string, std::set<std::string>> inbound_;
};

}  // namespace ultra::runtime::cognitive
