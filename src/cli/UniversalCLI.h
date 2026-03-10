#pragma once

#include "CommandOptions.h"
#include "InitCommand.h"
#include "../config/ConfigLoader.h"
#include "../core/ProjectType.h"
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::adapters {
class ProjectAdapter;
}

namespace ultra::cli {

class UniversalCLI {
 public:
  int execute(const std::string& command,
              const std::vector<std::string>& rawArgs);

  static bool isUniversalCommand(const std::string& command);

 private:
  int executeForTarget(const std::string& command,
                       const CommandOptions& options,
                       const std::filesystem::path& rootPath,
                       ultra::core::ProjectType projectType,
                       const std::string& targetLabel);

  int executeForModules(const std::string& command,
                        const CommandOptions& options,
                        const ultra::config::UltraConfig& config);

  int executeExec(const std::vector<std::string>& rawArgs,
                  const std::filesystem::path& rootPath);
  int executeDoctor(const std::vector<std::string>& rawArgs);

  static void dispatchOnAdapter(const std::string& command,
                                ultra::adapters::ProjectAdapter& adapter,
                                const CommandOptions& options);

  InitCommand m_initCommand;
};

}  // namespace ultra::cli
