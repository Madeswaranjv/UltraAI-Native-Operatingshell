#include "Logger.h"
#include <iostream>

namespace ultra::core {

LogLevel Logger::s_level = LogLevel::Info;
std::vector<ultra::types::StructuredError> Logger::s_errors;

void Logger::setLevel(LogLevel level) noexcept {
  s_level = level;
}

LogLevel Logger::level() noexcept {
  return s_level;
}

const char* Logger::levelTag(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warning:
      return "WARNING";
    case LogLevel::Error:
      return "ERROR";
  }
  return "INFO";
}

const char* Logger::categoryTag(LogCategory category) noexcept {
  switch (category) {
    case LogCategory::Scan:
      return "SCAN";
    case LogCategory::Graph:
      return "GRAPH";
    case LogCategory::Incremental:
      return "INCREMENTAL";
    case LogCategory::Build:
      return "BUILD";
    case LogCategory::Context:
      return "CONTEXT";
    case LogCategory::Patch:
      return "PATCH";
    case LogCategory::General:
    default:
      return "GENERAL";
  }
}

void Logger::log(LogLevel level, const std::string& message) {
  log(LogCategory::General, level, message);
}

void Logger::log(LogCategory category, LogLevel level,
                 const std::string& message) {
  if (static_cast<int>(level) < static_cast<int>(s_level)) {
    return;
  }

  if (level == LogLevel::Error) {
    std::cerr << "[ERROR] " << message << '\n';
    return;
  }

  if (level == LogLevel::Warning) {
    std::cerr << "[WARNING] " << message << '\n';
    return;
  }

  std::cout << "[" << levelTag(level) << "] [" << categoryTag(category) << "] "
            << message << '\n';
}

void Logger::info(const std::string& message) {
  log(LogCategory::General, LogLevel::Info, message);
}

void Logger::info(LogCategory category, const std::string& message) {
  log(category, LogLevel::Info, message);
}

void Logger::warning(const std::string& message) {
  log(LogCategory::General, LogLevel::Warning, message);
}

void Logger::warning(LogCategory category, const std::string& message) {
  log(category, LogLevel::Warning, message);
}

void Logger::error(const std::string& message) {
  log(LogCategory::General, LogLevel::Error, message);
}

void Logger::error(LogCategory category, const std::string& message) {
  log(category, LogLevel::Error, message);
}

void Logger::structured(const ultra::types::StructuredError& error) {
  s_errors.push_back(error);
  log(LogLevel::Error, "[" + error.errorType + "] " + error.message);
}

const std::vector<ultra::types::StructuredError>& Logger::collectedErrors() noexcept {
  return s_errors;
}

void Logger::clearErrors() noexcept {
  s_errors.clear();
}

}  // namespace ultra::core
