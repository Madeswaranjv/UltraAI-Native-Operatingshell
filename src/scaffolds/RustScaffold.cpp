#include "RustScaffold.h"
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

RustScaffold::RustScaffold(IScaffoldEnvironment& environment)
    : ScaffoldBase(environment) {}

void RustScaffold::generate(const std::string& name,
                            const ScaffoldOptions& options) {
  const std::filesystem::path projectRoot = options.destinationRoot / name;
  if (!ensureTargetAvailable(projectRoot, options)) {
    return;
  }

  if (!ensureTool("cargo", {"cargo --version"})) {
    return;
  }

  printInitStart(name);

  std::string command = "cargo new " + quoteArg(name);
  command = joinCommand(command, options.passthroughFlags);

  if (run(options.destinationRoot, command, options) != 0) {
    return;
  }

  if (!writeUltraConfig(projectRoot, "backend", "rust", options)) {
    return;
  }

  printEnterInstruction(name);
  succeed();
}

}  // namespace ultra::scaffolds
