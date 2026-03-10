#pragma once

#include "../types/Timestamp.h"
#include <external/json.hpp>

#include <string>

namespace ultra::intelligence {

/// Status of an individual execution node within a branch.
enum class NodeStatus : std::uint8_t {
  Pending = 0,
  Running = 1,
  Success = 2,
  Failed = 3
};

/// Represents a single reasoning or execution step within a branch.
struct ExecutionNode {
  /// Unique identifier of this execution step.
  std::string nodeId;

  /// The overarching branch this step belongs to.
  std::string branchId;

  /// The action taken (e.g., "Analyze Complexity", "Diff Compare").
  std::string action;

  /// Input payload or context given to this step.
  nlohmann::json input;

  /// Output payload resulting from this step.
  nlohmann::json output;

  /// Current status of the execution.
  NodeStatus status{NodeStatus::Pending};

  /// Time this step was initiated.
  ultra::types::Timestamp timestamp;

  /// Duration of execution in milliseconds.
  std::uint64_t durationMs{0};
};

}  // namespace ultra::intelligence
