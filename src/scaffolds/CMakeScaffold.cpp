#include "CMakeScaffold.h"
#include <filesystem>
#include <string>

namespace ultra::scaffolds {

CMakeScaffold::CMakeScaffold(IScaffoldEnvironment& environment)
    : ScaffoldBase(environment) {}

void CMakeScaffold::generate(const std::string& name,
                             const ScaffoldOptions& options) {
  const std::filesystem::path projectRoot = options.destinationRoot / name;
  if (!ensureTargetAvailable(projectRoot, options)) {
    return;
  }

  if (!ensureTool("cmake", {"cmake --version"})) {
    return;
  }

  printInitStart(name);

  if (!options.dryRun) {
    const std::filesystem::path srcDir = projectRoot / "src";
    if (!environment().createDirectories(srcDir)) {
      fail("Failed to create directory: " + srcDir.generic_string() + ".");
      return;
    }

    const std::string cmakeLists =
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(" +
        name +
        " LANGUAGES CXX)\n"
        "\n"
        "set(CMAKE_CXX_STANDARD 20)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "set(CMAKE_CXX_EXTENSIONS OFF)\n"
        "\n"
        "add_executable(" +
        name +
        " src/main.cpp)\n";

    if (!environment().writeTextFile(projectRoot / "CMakeLists.txt",
                                     cmakeLists)) {
      fail("Failed to write CMakeLists.txt.");
      return;
    }

    const std::string mainCpp =
        "#include <iostream>\n"
        "\n"
        "int main() {\n"
        "  std::cout << \"Ultra CMake scaffold ready.\\n\";\n"
        "  return 0;\n"
        "}\n";

    if (!environment().writeTextFile(srcDir / "main.cpp", mainCpp)) {
      fail("Failed to write src/main.cpp.");
      return;
    }
  }

  if (!writeUltraConfig(projectRoot, "backend", "cmake", options)) {
    return;
  }

  printEnterInstruction(name);
  succeed();
}

}  // namespace ultra::scaffolds
