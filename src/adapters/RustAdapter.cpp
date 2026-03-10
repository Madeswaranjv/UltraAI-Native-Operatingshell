#include "RustAdapter.h"
#include "AdapterCommandRunner.h"
#include <exception>
#include <filesystem>
#include <iostream>
#include <utility>

namespace ultra::adapters {

namespace {

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

RustAdapter::RustAdapter(std::filesystem::path rootPath)
    : m_rootPath(std::move(rootPath)) {}

void RustAdapter::install(const ultra::cli::CommandOptions& options) {
  std::string command = "cargo fetch";
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void RustAdapter::dev(const ultra::cli::CommandOptions& options) {
  std::string command = "cargo run";
  if (options.release) {
    command += " --release";
  }
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void RustAdapter::build(const ultra::cli::CommandOptions& options) {
  std::string command = "cargo build";
  if (options.release) {
    command += " --release";
  }
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void RustAdapter::test(const ultra::cli::CommandOptions& options) {
  std::string command = "cargo test";
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void RustAdapter::run(const ultra::cli::CommandOptions& options) {
  std::string command = "cargo run";
  if (options.release) {
    command += " --release";
  }
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void RustAdapter::clean(const ultra::cli::CommandOptions& options) {
  std::string command = "cargo clean";
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }

  m_lastExitCode = execute(command, options);
  if (m_lastExitCode != 0) {
    return;
  }

  if (!options.deep) {
    return;
  }

  bool removedAnything = false;
  if (!removeEntry(m_rootPath / "target", "target", options, removedAnything)) {
    m_lastExitCode = 1;
    return;
  }

  if (!removedAnything) {
    std::cout << "[CLEAN] Nothing to clean.\n";
  }
}

int RustAdapter::lastExitCode() const noexcept {
  return m_lastExitCode;
}

bool RustAdapter::ensureCargoAvailable(
    const ultra::cli::CommandOptions& options) const {
  if (options.dryRun) {
    return true;
  }

  if (isToolAvailable("cargo")) {
    return true;
  }

  std::cout << "[ERROR] Cargo not found in PATH.\n";
  std::cout << "Install Rust from https://rustup.rs\n";
  return false;
}

int RustAdapter::execute(const std::string& command,
                         const ultra::cli::CommandOptions& options) {
  if (!ensureCargoAvailable(options)) {
    return 1;
  }
  return runCommand(m_rootPath, command, options);
}

}  // namespace ultra::adapters
