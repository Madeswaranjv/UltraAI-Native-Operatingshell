#include "BuildEngine.h"
#include "../platform/IProcessExecutor.h"
#if defined(_WIN32)
#include "../platform/WindowsProcessExecutor.h"
#else
#include "../platform/UnixProcessExecutor.h"
#endif
#include "../core/Logger.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace ultra::build {

BuildEngine::BuildEngine()
#if defined(_WIN32)
    : executor_(std::make_unique<ultra::platform::WindowsProcessExecutor>()) {}
#else
    : executor_(std::make_unique<ultra::platform::UnixProcessExecutor>()) {}
#endif

BuildEngine::BuildEngine(
    std::unique_ptr<ultra::platform::IProcessExecutor> executor)
    : executor_(std::move(executor)) {}

BuildEngine::~BuildEngine() = default;

int BuildEngine::fullBuild(const std::filesystem::path& projectPath) {
  if (!executor_) {
    ultra::core::Logger::error(ultra::core::LogCategory::Build,
                               "No process executor configured.");
    return 1;
  }

  std::filesystem::path cmakeLists = projectPath / "CMakeLists.txt";
  if (!std::filesystem::exists(cmakeLists) ||
      !std::filesystem::is_regular_file(cmakeLists)) {
    ultra::core::Logger::error(ultra::core::LogCategory::Build,
                               "CMakeLists.txt not found: " +
                              projectPath.string());
    return 1;
  }
  std::filesystem::path buildDir = projectPath / "build";
  std::cout << "Build Started\n";
  if (!std::filesystem::exists(buildDir) ||
      !std::filesystem::is_directory(buildDir)) {
    std::cout << "Configuring project...\n";
    std::ostringstream cmd;
    cmd << "cmake -S \"" << projectPath.string() << "\" -B \""
        << buildDir.string() << "\"";
    int code = executor_->run(cmd.str());
    if (code != 0) {
      std::cout << "Build failed with exit code: " << code << '\n';
      return code;
    }
  }
  std::cout << "Building project...\n";
  std::ostringstream buildCmd;
  buildCmd << "cmake --build \"" << buildDir.string()
           << "\" --config Release";
  int code = executor_->run(buildCmd.str());
  if (code != 0) {
    std::cout << "Build failed with exit code: " << code << '\n';
    return code;
  }
  std::cout << "Build completed successfully.\n";
  return 0;
}

int BuildEngine::incrementalBuild(
    const std::filesystem::path& projectPath,
    const std::vector<std::string>& rebuildSet) {
  (void)rebuildSet;
  return fullBuild(projectPath);
}

int BuildEngine::fastIncrementalBuild(
    const std::filesystem::path& projectPath,
    const std::vector<std::string>& rebuildSet,
    std::size_t allCppCount) {
  auto delegateToFull = [this, &projectPath]() {
    ultra::core::Logger::info(ultra::core::LogCategory::Build,
                              "Fast mode delegated to CMake. True per-file "
                              "compilation not implemented yet.");
    return fullBuild(projectPath);
  };
  std::size_t cppInSet = 0;
  for (const std::string& pathStr : rebuildSet) {
    if (std::filesystem::path(pathStr).extension() == ".cpp") ++cppInSet;
  }
  if (allCppCount > 0 && cppInSet >= allCppCount) {
    ultra::core::Logger::info(ultra::core::LogCategory::Build,
                              "Rebuild set contains all .cpp files; using full build.");
    return delegateToFull();
  }
  if (cppInSet > 0) {
    ultra::core::Logger::info(ultra::core::LogCategory::Build,
                              "Partial rebuild detected.");
  }
  std::filesystem::path buildDir = projectPath / "build";
  std::filesystem::path cmakeLists = projectPath / "CMakeLists.txt";
  if (!std::filesystem::exists(cmakeLists) ||
      !std::filesystem::is_regular_file(cmakeLists)) {
    ultra::core::Logger::error(ultra::core::LogCategory::Build,
                               "CMakeLists.txt not found.");
    return delegateToFull();
  }
  if (!std::filesystem::exists(buildDir) ||
      !std::filesystem::is_directory(buildDir)) {
    return delegateToFull();
  }
  std::filesystem::path objDir = buildDir / "ultra.dir" / "Release";
  std::filesystem::path outExe = buildDir / "Release" / "ultra.exe";
  if (!std::filesystem::exists(objDir)) {
    std::filesystem::path alt = buildDir / "x64" / "Release";
    if (std::filesystem::exists(alt)) {
      objDir = alt;
      outExe = buildDir / "x64" / "Release" / "ultra.exe";
    }
  }
  std::string includeArg =
      "/I \"" + projectPath.string() + "\" /I \"" +
      (projectPath / "include").string() + "\"";
  for (const std::string& pathStr : rebuildSet) {
    std::filesystem::path p(pathStr);
    if (p.extension() != ".cpp") continue;
    if (!std::filesystem::exists(p) || !std::filesystem::is_regular_file(p))
      continue;
    std::filesystem::path objPath = objDir / (p.stem().string() + ".obj");
    std::ostringstream cmd;
    cmd << "cl.exe /nologo /c /EHsc /std:c++20 " << includeArg << " /Fo \""
        << objPath.string() << "\" \"" << pathStr << "\"";
    int code = executor_->run(cmd.str());
    if (code != 0) {
      std::cout << "build-fast: compile failed, falling back to full build.\n";
      return delegateToFull();
    }
  }
  std::vector<std::string> objFiles;
  try {
    for (auto it = std::filesystem::directory_iterator(objDir);
         it != std::filesystem::directory_iterator(); ++it) {
      if (it->path().extension() == ".obj")
        objFiles.push_back(it->path().string());
    }
  } catch (...) {
    return delegateToFull();
  }
  if (objFiles.empty()) {
    return delegateToFull();
  }
  std::ostringstream linkCmd;
  linkCmd << "link.exe /nologo /OUT:\"" << outExe.string() << "\"";
  for (const std::string& o : objFiles) linkCmd << " \"" << o << "\"";
  int code = executor_->run(linkCmd.str());
  if (code != 0) {
    std::cout << "build-fast: link failed, falling back to full build.\n";
    return delegateToFull();
  }
  std::cout << "build-fast completed successfully.\n";
  return 0;
}

}  // namespace ultra::build
