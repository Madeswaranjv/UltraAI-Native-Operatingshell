#include "UnixProcessExecutor.h"
#include "../core/Logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace ultra::platform {

ProcessResult UnixProcessExecutor::execute(const std::string& command) {
  ProcessResult result;
  ultra::core::Logger::info(ultra::core::LogCategory::Build,
                            "Executing: " + command);

  auto start = std::chrono::steady_clock::now();

#if defined(_WIN32)
  // Should not be compiled on Windows, but guard just in case.
  (void)command;
  result.exitCode = -1;
  result.launched = false;
#else
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

  // On Unix, std::system returns the full status; extract actual exit code.
  if (WIFEXITED(code)) {
    result.exitCode = WEXITSTATUS(code);
  } else {
    result.exitCode = code;
  }
  result.launched = true;
#endif

  return result;
}

int UnixProcessExecutor::run(const std::string& command) {
  return execute(command).exitCode;
}

}  // namespace ultra::platform
