#pragma once

#include "ExecutionKernel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ultra::runtime::cognitive {

enum class FailureClass : std::uint8_t {
  TRANSIENT_ERROR = 0U,
  VALIDATION_ERROR = 1U,
  DEPENDENCY_ERROR = 2U,
  CRITICAL_ERROR = 3U
};

[[nodiscard]] const char* toString(FailureClass failureClass) noexcept;

enum class RecoveryAction : std::uint8_t {
  RETRY_TASK = 0U,
  SKIP_TASK = 1U,
  REPLAN_REQUIRED = 2U,
  ABORT_LOOP = 3U
};

[[nodiscard]] const char* toString(RecoveryAction action) noexcept;

struct DependencyState {
  bool hasPendingTasks{false};
  std::size_t failedTaskCount{0U};
};

struct FailureContext {
  std::string task_id;
  ::ultra::runtime::Result execution_result{};
  std::size_t retry_count{0U};
  std::size_t retry_limit{0U};
  std::optional<DependencyState> dependency_state{};
  std::vector<std::string> recovery_patterns;
  std::optional<RecoveryAction> memory_action{};
  bool repeated_failure_detected{false};
};

class FailureRecoveryEngine {
 public:
  [[nodiscard]] RecoveryAction decide(const FailureContext& ctx) const;

 private:
  [[nodiscard]] FailureClass classify(const FailureContext& ctx) const;
};

}  // namespace ultra::runtime::cognitive