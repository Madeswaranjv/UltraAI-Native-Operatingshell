#include "DjangoScaffold.h"
#include <filesystem>
#include <iostream>

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

DjangoScaffold::DjangoScaffold(IScaffoldEnvironment& environment)
    : ScaffoldBase(environment) {}

void DjangoScaffold::generate(const std::string& name,
                              const ScaffoldOptions& options) {
  const std::filesystem::path projectRoot = options.destinationRoot / name;
  if (!ensureTargetAvailable(projectRoot, options)) {
    return;
  }

  if (!ensureTool("python", {"python --version", "py -3 --version"})) {
    return;
  }

  if (!environment().isToolAvailable({"django-admin --version",
                                      "python -m django --version",
                                      "py -3 -m django --version"})) {
    fail("django-admin not found in PATH.");
    std::cout << "Run: pip install django\n";
    return;
  }

  printInitStart(name);

  std::string command = "django-admin startproject " + quoteArg(name);
  if (!options.templateName.empty()) {
    command += " --template " + quoteArg(options.templateName);
  }
  command = joinCommand(command, options.passthroughFlags);

  if (run(options.destinationRoot, command, options) != 0) {
    return;
  }

  if (!writeUltraConfig(projectRoot, "backend", "python", options)) {
    return;
  }

  printEnterInstruction(name);
  succeed();
}

}  // namespace ultra::scaffolds
