#include "ScaffoldBase.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace ultra::scaffolds {

namespace {

std::string quoteForCmd(const std::string& value) {
  if (value.empty()) {
    return "\"\"";
  }

  if (value.find_first_of(" \t\"") == std::string::npos) {
    return value;
  }

  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char c : value) {
    if (c == '\"') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return "\"" + escaped + "\"";
}

std::string normalizedPath(const std::filesystem::path& path) {
  std::string value = path.lexically_normal().generic_string();
  const bool isWindowsDriveRoot =
      value.size() == 3 && std::isalpha(static_cast<unsigned char>(value[0])) &&
      value[1] == ':' && value[2] == '/';
  if (!isWindowsDriveRoot && value.size() > 1 && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

}  // namespace

std::filesystem::path DefaultScaffoldEnvironment::currentPath() const {
  return std::filesystem::current_path();
}

bool DefaultScaffoldEnvironment::pathExists(
    const std::filesystem::path& path) const {
  return std::filesystem::exists(path);
}

bool DefaultScaffoldEnvironment::createDirectories(
    const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    return false;
  }
  return true;
}

bool DefaultScaffoldEnvironment::writeTextFile(
    const std::filesystem::path& path,
    const std::string& content) {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << content;
  return static_cast<bool>(out);
}

bool DefaultScaffoldEnvironment::isToolAvailable(
    const std::vector<std::string>& probeCommands) const {
  for (const std::string& probe : probeCommands) {
    const std::string shell = "cmd /c \"" + probe + " >nul 2>&1\"";
    const int code = std::system(shell.c_str());
    if (code == 0) {
      return true;
    }
  }
  return false;
}

int DefaultScaffoldEnvironment::runCommand(
    const std::filesystem::path& workingDirectory,
    const std::string& command,
    const ScaffoldOptions& options) {
  std::cout << "[exec] (" << normalizedPath(workingDirectory) << ") " << command
            << '\n';

  if (options.dryRun) {
    return 0;
  }

  std::error_code ec;
  const std::filesystem::path originalDirectory =
      std::filesystem::current_path(ec);
  if (ec) {
    return 1;
  }

  std::filesystem::current_path(workingDirectory, ec);
  if (ec) {
    return 1;
  }

  const int code = std::system(command.c_str());

  std::error_code restoreEc;
  std::filesystem::current_path(originalDirectory, restoreEc);
  (void)restoreEc;

  if (code < 0) {
    return 1;
  }

  return code;
}

ScaffoldBase::ScaffoldBase(IScaffoldEnvironment& environment)
    : m_environment(environment) {}

int ScaffoldBase::lastExitCode() const noexcept {
  return m_lastExitCode;
}

IScaffoldEnvironment& ScaffoldBase::environment() noexcept {
  return m_environment;
}

const IScaffoldEnvironment& ScaffoldBase::environment() const noexcept {
  return m_environment;
}

bool ScaffoldBase::ensureTargetAvailable(
    const std::filesystem::path& targetDirectory,
    const ScaffoldOptions& options) {
  if (!options.force && environment().pathExists(targetDirectory)) {
    fail("Target path already exists: " + normalizedPath(targetDirectory) +
         " (use --force to continue).");
    return false;
  }
  return true;
}

bool ScaffoldBase::ensureTool(const std::string& toolDisplayName,
                              const std::vector<std::string>& probeCommands) {
  if (environment().isToolAvailable(probeCommands)) {
    return true;
  }

  fail("Required tool not found: " + toolDisplayName + ".");
  return false;
}

bool ScaffoldBase::writeUltraConfig(const std::filesystem::path& projectRoot,
                                    const std::string& moduleName,
                                    const std::string& moduleType,
                                    const ScaffoldOptions& options) {
  const std::filesystem::path configPath = projectRoot / "ultra.config.json";
  const std::string config =
      "{\n"
      "  \"version\": \"1.0\",\n"
      "  \"modules\": {\n"
      "    \"" +
      moduleName +
      "\": { \"path\": \".\", \"type\": \"" + moduleType +
      "\" }\n"
      "  }\n"
      "}\n";

  if (options.verbose || options.dryRun) {
    std::cout << "[INIT] Writing " << normalizedPath(configPath) << '\n';
  }

  if (options.dryRun) {
    return true;
  }

  if (!environment().writeTextFile(configPath, config)) {
    fail("Failed to write " + normalizedPath(configPath) + ".");
    return false;
  }

  return true;
}

void ScaffoldBase::printInitStart(const std::string& projectName) const {
  (void)projectName;
  std::cout << "[INIT] Creating project...\n";
}

void ScaffoldBase::printEnterInstruction(const std::string& projectName) const {
  std::cout << "[INIT] Project created: " << projectName << '\n';
  std::cout << "Next: cd " << quoteArg(projectName) << '\n';
}

int ScaffoldBase::run(const std::filesystem::path& workingDirectory,
                      const std::string& command,
                      const ScaffoldOptions& options) {
  const int code = environment().runCommand(workingDirectory, command, options);
  if (code != 0) {
    fail("Scaffolding failed.");
  }
  return code;
}

void ScaffoldBase::fail(const std::string& message) {
  std::cout << "[ERROR] " << message << '\n';
  m_lastExitCode = 1;
}

void ScaffoldBase::succeed() {
  m_lastExitCode = 0;
}

std::string ScaffoldBase::quoteArg(const std::string& value) {
  return quoteForCmd(value);
}

}  // namespace ultra::scaffolds
