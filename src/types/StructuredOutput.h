#pragma once

#include "StructuredError.h"
#include "Timestamp.h"
#include "external/json.hpp"

#include <string>
#include <vector>

namespace ultra::types {

/// Status of a command or operation output.
enum class OutputStatus {
  Success,
  Failure,
  Partial,   // Some sub-tasks succeeded, some failed.
  Skipped    // Operation was skipped (e.g. nothing to do).
};

/// Convert OutputStatus to string.
inline std::string outputStatusToString(OutputStatus status) {
  switch (status) {
    case OutputStatus::Success: return "success";
    case OutputStatus::Failure: return "failure";
    case OutputStatus::Partial: return "partial";
    case OutputStatus::Skipped: return "skipped";
  }
  return "unknown";
}

/// Structured output object wrapping all Ultra command results.
///
/// In JSON mode (--json), this is the top-level output of every command.
/// Example:
/// {
///   "status": "success",
///   "command": "build",
///   "data": { ... },
///   "errors": [],
///   "metadata": { "duration_ms": 1234, "files_processed": 47 },
///   "timestamp": "2026-02-21T16:30:00.000Z"
/// }
struct StructuredOutput {
  /// Overall status of the operation.
  OutputStatus status{OutputStatus::Success};

  /// The command that produced this output.
  std::string command;

  /// Structured result data (command-specific).
  nlohmann::json data;

  /// Any errors encountered during execution.
  std::vector<StructuredError> errors;

  /// Execution metadata.
  nlohmann::json metadata;

  /// When this output was produced.
  Timestamp timestamp;
};

}  // namespace ultra::types
