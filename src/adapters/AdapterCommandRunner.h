#pragma once

#include "../cli/CommandOptions.h"
#include <filesystem>
#include <string>

namespace ultra::adapters {

int runCommand(const std::filesystem::path& workingDirectory,
               const std::string& command,
               const ultra::cli::CommandOptions& options);

bool isToolAvailable(const std::string& toolName);

}  // namespace ultra::adapters
