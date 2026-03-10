#pragma once

#include "../scaffolds/ScaffoldBase.h"
#include <memory>
#include <string>
#include <vector>

namespace ultra::cli {

class InitCommand {
 public:
  InitCommand();
  explicit InitCommand(ultra::scaffolds::IScaffoldEnvironment& environment);

  int execute(const std::vector<std::string>& args);

  static std::string supportedStackMessage();

 private:
  struct ParsedInitArgs {
    bool ok{false};
    std::string stack;
    std::string projectName;
    ultra::scaffolds::ScaffoldOptions options;
    std::string error;
  };

  ParsedInitArgs parseArgs(const std::vector<std::string>& args) const;

  std::unique_ptr<ultra::scaffolds::IScaffoldEnvironment> m_ownedEnvironment;
  ultra::scaffolds::IScaffoldEnvironment* m_environment{nullptr};
};

}  // namespace ultra::cli
