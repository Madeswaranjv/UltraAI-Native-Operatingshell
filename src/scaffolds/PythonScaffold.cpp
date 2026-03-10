#include "PythonScaffold.h"
#include <filesystem>
#include <string>

namespace ultra::scaffolds {

PythonScaffold::PythonScaffold(IScaffoldEnvironment& environment)
    : ScaffoldBase(environment) {}

void PythonScaffold::generate(const std::string& name,
                              const ScaffoldOptions& options) {
  const std::filesystem::path projectRoot = options.destinationRoot / name;
  if (!ensureTargetAvailable(projectRoot, options)) {
    return;
  }

  if (!ensureTool("python", {"python --version", "py -3 --version"})) {
    return;
  }

  printInitStart(name);

  if (!options.dryRun) {
    if (!environment().createDirectories(projectRoot)) {
      fail("Failed to create directory: " + projectRoot.generic_string() + ".");
      return;
    }

    const std::filesystem::path mainPath = projectRoot / "main.py";
    const std::string mainContent =
        "def main() -> None:\n"
        "    print(\"Ultra Python scaffold ready.\")\n"
        "\n"
        "\n"
        "if __name__ == \"__main__\":\n"
        "    main()\n";

    if (!environment().writeTextFile(mainPath, mainContent)) {
      fail("Failed to write " + mainPath.generic_string() + ".");
      return;
    }
  }

  if (!writeUltraConfig(projectRoot, "backend", "python", options)) {
    return;
  }

  printEnterInstruction(name);
  succeed();
}

}  // namespace ultra::scaffolds
