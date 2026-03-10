#include "WindowsProcessExecutor.h"
#include "../core/Logger.h"

#include <chrono>
#include <cstdlib>
#include <string>

namespace ultra::platform {

ProcessResult WindowsProcessExecutor::execute(const std::string& command) {
  ProcessResult result;
  ultra::core::Logger::info(ultra::core::LogCategory::Build,
                            "Executing: " + command);

  auto start = std::chrono::steady_clock::now();
  int code = std::system(command.c_str());
  auto end = std::chrono::steady_clock::now();

  result.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          end - start).count();

  if (code < 0) {
    ultra::core::Logger::error(ultra::core::LogCategory::Build,
                               "Process execution failed.");
    result.exitCode = -1;
    result.launched = false;
    return result;
  }

  result.exitCode = code;
  result.launched = true;
  return result;
}

int WindowsProcessExecutor::run(const std::string& command) {
  return execute(command).exitCode;
}

}  // namespace ultra::platform
