#include "AdapterCommandRunner.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace ultra::adapters {

namespace {

std::string normalizedPath(const std::filesystem::path& path) {
  std::string value = path.lexically_normal().generic_string();
  const bool isWindowsDriveRoot =
      value.size() == 3 && std::isalpha(static_cast<unsigned char>(value[0])) &&
      value[1] == ':' && value[2] == '/';
  if (!isWindowsDriveRoot && value.size() > 1 && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

}  // namespace

bool isToolAvailable(const std::string& toolName) {
  if (toolName.empty()) {
    return false;
  }

  const std::string probe = toolName + " --version >nul 2>&1";
  const int code = std::system(probe.c_str());
  return code == 0;
}

int runCommand(const std::filesystem::path& workingDirectory,
               const std::string& command,
               const ultra::cli::CommandOptions& options) {
  std::cout << "[exec] (" << normalizedPath(workingDirectory) << ") " << command
            << '\n';

  if (options.dryRun) {
    return 0;
  }

  std::error_code ec;
  const std::filesystem::path originalDirectory =
      std::filesystem::current_path(ec);
  if (ec) {
    std::cout << "[ERROR] Failed to read current working directory.\n";
    return 1;
  }

  std::filesystem::current_path(workingDirectory, ec);
  if (ec) {
    std::cout << "[ERROR] Failed to switch to: " << normalizedPath(workingDirectory)
              << '\n';
    return 1;
  }

  const int code = std::system(command.c_str());

  std::error_code restoreEc;
  std::filesystem::current_path(originalDirectory, restoreEc);
  if (restoreEc) {
    std::cout << "[ERROR] Failed to restore original working directory.\n";
  }

  if (code < 0) {
    std::cout << "[ERROR] Failed to execute command: " << command << '\n';
    return 1;
  }

  if (code != 0) {
    std::cout << "[ERROR] Command failed (exit code " << code << ").\n";
  }

  return code;
}

}  // namespace ultra::adapters
