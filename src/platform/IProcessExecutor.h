#pragma once

#include <cstdint>
#include <string>

namespace ultra::platform {

/// Structured result from a process execution.
///
/// Replaces the raw int return from the old IProcessExecutor::run().
/// All process execution in Ultra produces this structured result.
struct ProcessResult {
  /// Process exit code (0 = success).
  int exitCode{-1};

  /// Captured stdout (may be empty if not captured).
  std::string stdOut;

  /// Captured stderr (may be empty if not captured).
  std::string stdErr;

  /// Execution duration in milliseconds.
  std::int64_t durationMs{0};

  /// Whether the process was actually launched.
  bool launched{false};

  /// Convenience: did the process succeed?
  bool success() const noexcept { return launched && exitCode == 0; }
};

/// Interface for executing system processes.
///
/// Platform-specific implementations provide the actual execution.
/// The interface guarantees structured ProcessResult returns.
class IProcessExecutor {
 public:
  /// Execute a command and return a structured result.
  virtual ProcessResult execute(const std::string& command) = 0;

  /// Legacy compatibility: execute and return just the exit code.
  virtual int run(const std::string& command) {
    return execute(command).exitCode;
  }

  virtual ~IProcessExecutor() = default;
};

}  // namespace ultra::platform
