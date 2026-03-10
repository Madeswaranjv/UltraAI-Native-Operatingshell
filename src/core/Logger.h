#pragma once

#include "../types/StructuredError.h"

#include <string>
#include <vector>

namespace ultra::core {

enum class LogLevel { Info, Warning, Error };

enum class LogCategory {
  Scan,
  Graph,
  Incremental,
  Build,
  Context,
  Patch,
  General
};

class Logger {
 public:
  static void setLevel(LogLevel level) noexcept;
  static LogLevel level() noexcept;
  static void log(LogLevel level, const std::string& message);
  static void log(LogCategory category, LogLevel level,
                  const std::string& message);
  static void info(const std::string& message);
  static void info(LogCategory category, const std::string& message);
  static void warning(const std::string& message);
  static void warning(LogCategory category, const std::string& message);
  static void error(const std::string& message);
  static void error(LogCategory category, const std::string& message);

  /// Emit a structured error (also logs it as a regular error).
  static void structured(const ultra::types::StructuredError& error);

  /// Collect all structured errors since the last clear.
  static const std::vector<ultra::types::StructuredError>& collectedErrors() noexcept;

  /// Clear the collected error list.
  static void clearErrors() noexcept;

 private:
  static LogLevel s_level;
  static std::vector<ultra::types::StructuredError> s_errors;
  static const char* levelTag(LogLevel level) noexcept;
  static const char* categoryTag(LogCategory category) noexcept;
};

}  // namespace ultra::core
