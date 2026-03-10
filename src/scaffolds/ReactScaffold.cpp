#include "ReactScaffold.h"
#include <filesystem>

namespace ultra::scaffolds {

namespace {

std::string joinCommand(const std::string& base,
                        const std::string& maybeFlags) {
  if (maybeFlags.empty()) {
    return base;
  }
  return base + " " + maybeFlags;
}

}  // namespace

ReactScaffold::ReactScaffold(IScaffoldEnvironment& environment)
    : ScaffoldBase(environment) {}

void ReactScaffold::generate(const std::string& name,
                             const ScaffoldOptions& options) {
  const std::filesystem::path projectRoot = options.destinationRoot / name;
  if (!ensureTargetAvailable(projectRoot, options)) {
    return;
  }

  if (!ensureTool("npx", {"npx --version"})) {
    return;
  }

  printInitStart(name);

  const std::string templateName =
      options.templateName.empty() ? "react" : options.templateName;

  std::string command = "npx --yes create-vite@latest " + quoteArg(name) +
                        " --template " + quoteArg(templateName);
  command = joinCommand(command, options.passthroughFlags);

  if (run(options.destinationRoot, command, options) != 0) {
    return;
  }

  if (!writeUltraConfig(projectRoot, "frontend", "react", options)) {
    return;
  }

  printEnterInstruction(name);
  succeed();
}

}  // namespace ultra::scaffolds
