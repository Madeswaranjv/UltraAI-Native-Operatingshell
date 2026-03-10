#include "NextScaffold.h"
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

NextScaffold::NextScaffold(IScaffoldEnvironment& environment)
    : ScaffoldBase(environment) {}

void NextScaffold::generate(const std::string& name,
                            const ScaffoldOptions& options) {
  const std::filesystem::path projectRoot = options.destinationRoot / name;
  if (!ensureTargetAvailable(projectRoot, options)) {
    return;
  }

  if (!ensureTool("npx", {"npx --version"})) {
    return;
  }

  printInitStart(name);

  std::string command =
      "npx --yes create-next-app@latest " + quoteArg(name) +
      " --use-npm --ts --eslint --tailwind --app --src-dir --import-alias "
      "\"@/*\" --yes";
  if (!options.templateName.empty()) {
    command += " --example " + quoteArg(options.templateName);
  }
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
