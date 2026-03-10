#include "ReactAdapter.h"
#include "AdapterCommandRunner.h"
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

namespace ultra::adapters {

namespace {

std::string join(const std::vector<std::string>& args) {
  std::ostringstream out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      out << ' ';
    }
    out << args[i];
  }
  return out.str();
}

std::string withScriptArgs(std::string baseCommand,
                           const std::vector<std::string>& scriptArgs,
                           const std::string& nativeArgs) {
  if (scriptArgs.empty() && nativeArgs.empty()) {
    return baseCommand;
  }

  baseCommand += " --";
  if (!scriptArgs.empty()) {
    baseCommand += " ";
    baseCommand += join(scriptArgs);
  }
  if (!nativeArgs.empty()) {
    baseCommand += " ";
    baseCommand += nativeArgs;
  }
  return baseCommand;
}

bool removeEntry(const std::filesystem::path& absolutePath,
                 const std::string& displayName,
                 const ultra::cli::CommandOptions& options,
                 bool& removedAnything) {
  try {
    if (!std::filesystem::exists(absolutePath)) {
      return true;
    }

    if (options.dryRun) {
      std::cout << "[CLEAN] Would remove " << displayName << '\n';
      removedAnything = true;
      return true;
    }

    if (std::filesystem::is_directory(absolutePath)) {
      std::filesystem::remove_all(absolutePath);
    } else {
      std::filesystem::remove(absolutePath);
    }

    std::cout << "[CLEAN] Removed " << displayName << '\n';
    removedAnything = true;
    return true;
  } catch (const std::exception& ex) {
    std::cout << "[ERROR] Failed to remove " << displayName << ": "
              << ex.what() << '\n';
    return false;
  } catch (...) {
    std::cout << "[ERROR] Failed to remove " << displayName << '\n';
    return false;
  }
}

}  // namespace

ReactAdapter::ReactAdapter(std::filesystem::path rootPath)
    : m_rootPath(std::move(rootPath)) {}

void ReactAdapter::install(const ultra::cli::CommandOptions& options) {
  std::string command = "npm install";
  if (options.force) {
    command += " --force";
  }
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void ReactAdapter::dev(const ultra::cli::CommandOptions& options) {
  std::vector<std::string> scriptArgs;
  if (options.watch) {
    scriptArgs.push_back("--watch");
  }
  m_lastExitCode = execute(
      withScriptArgs("npm run dev", scriptArgs, options.nativeArgs), options);
}

void ReactAdapter::build(const ultra::cli::CommandOptions& options) {
  std::vector<std::string> scriptArgs;
  if (options.release) {
    scriptArgs.push_back("--mode");
    scriptArgs.push_back("production");
  }
  if (options.debug) {
    scriptArgs.push_back("--mode");
    scriptArgs.push_back("development");
  }
  if (options.watch) {
    scriptArgs.push_back("--watch");
  }
  m_lastExitCode = execute(
      withScriptArgs("npm run build", scriptArgs, options.nativeArgs),
      options);
}

void ReactAdapter::test(const ultra::cli::CommandOptions& options) {
  std::vector<std::string> scriptArgs;
  if (!options.watch) {
    scriptArgs.push_back("--watch=false");
  }
  m_lastExitCode = execute(
      withScriptArgs("npm run test", scriptArgs, options.nativeArgs),
      options);
}

void ReactAdapter::run(const ultra::cli::CommandOptions& options) {
  std::vector<std::string> scriptArgs;
  if (options.watch) {
    scriptArgs.push_back("--watch");
  }
  m_lastExitCode = execute(
      withScriptArgs("npm run start", scriptArgs, options.nativeArgs),
      options);
}

void ReactAdapter::clean(const ultra::cli::CommandOptions& options) {
  bool removedAnything = false;
  const bool nextProject = isNextProject();

  if (nextProject) {
    if (!removeEntry(m_rootPath / ".next", ".next", options, removedAnything)) {
      m_lastExitCode = 1;
      return;
    }
    if (options.deep) {
      if (!removeEntry(m_rootPath / "node_modules", "node_modules", options,
                       removedAnything)) {
        m_lastExitCode = 1;
        return;
      }
    }
  } else {
    if (!removeEntry(m_rootPath / "dist", "dist", options, removedAnything)) {
      m_lastExitCode = 1;
      return;
    }
    if (options.deep) {
      if (!removeEntry(m_rootPath / "node_modules", "node_modules", options,
                       removedAnything)) {
        m_lastExitCode = 1;
        return;
      }
      if (!removeEntry(m_rootPath / "package-lock.json", "package-lock.json",
                       options, removedAnything)) {
        m_lastExitCode = 1;
        return;
      }
    }
  }

  if (!removedAnything) {
    std::cout << "[CLEAN] Nothing to clean.\n";
  }

  m_lastExitCode = 0;
}

int ReactAdapter::lastExitCode() const noexcept {
  return m_lastExitCode;
}

bool ReactAdapter::selfCheck(bool verbose) const {
  const bool npmInstalled = isToolAvailable("npm");
  const bool packageJsonPresent =
      std::filesystem::exists(m_rootPath / "package.json");

  if (verbose) {
    std::cout << "[doctor] react tool='npm' "
              << (npmInstalled ? "OK" : "Missing") << '\n';
    std::cout << "[doctor] react file='package.json' "
              << (packageJsonPresent ? "OK" : "Missing") << '\n';
  }

  return npmInstalled && packageJsonPresent;
}

bool ReactAdapter::ensureNpmAvailable(
    const ultra::cli::CommandOptions& options) const {
  if (options.dryRun) {
    return true;
  }

  if (isToolAvailable("npm")) {
    return true;
  }

  std::cout << "[ERROR] npm not found in PATH.\n";
  std::cout << "Please install Node.js from https://nodejs.org\n";
  return false;
}

bool ReactAdapter::isNextProject() const {
  if (std::filesystem::exists(m_rootPath / "next.config.js") ||
      std::filesystem::exists(m_rootPath / "next.config.mjs") ||
      std::filesystem::exists(m_rootPath / "next.config.ts")) {
    return true;
  }

  const std::filesystem::path packageJson = m_rootPath / "package.json";
  if (!std::filesystem::exists(packageJson)) {
    return false;
  }

  std::ifstream input(packageJson);
  if (!input) {
    return false;
  }

  std::ostringstream content;
  content << input.rdbuf();
  return content.str().find("\"next\"") != std::string::npos;
}

int ReactAdapter::execute(const std::string& command,
                          const ultra::cli::CommandOptions& options) {
  if (!ensureNpmAvailable(options)) {
    return 1;
  }
  return runCommand(m_rootPath, command, options);
}

}  // namespace ultra::adapters
