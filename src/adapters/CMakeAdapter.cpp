#include "CMakeAdapter.h"
#include "AdapterCommandRunner.h"
#include <exception>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ultra::adapters {

namespace {

#ifdef _WIN32
void clearReadonlyAttribute(const std::filesystem::path& absolutePath) {
  const std::wstring widePath = absolutePath.wstring();
  const DWORD attributes = ::GetFileAttributesW(widePath.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  if ((attributes & FILE_ATTRIBUTE_READONLY) != 0U) {
    (void)::SetFileAttributesW(widePath.c_str(),
                               attributes & ~FILE_ATTRIBUTE_READONLY);
  }
}
#endif

void makeWritableRecursive(const std::filesystem::path& absolutePath) {
  std::error_code ec;
  if (!std::filesystem::exists(absolutePath, ec) || ec) {
    return;
  }

#ifdef _WIN32
  clearReadonlyAttribute(absolutePath);
#endif
  std::filesystem::permissions(absolutePath, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::add, ec);
  if (!std::filesystem::is_directory(absolutePath, ec) || ec) {
    return;
  }

  for (std::filesystem::recursive_directory_iterator it(
           absolutePath, std::filesystem::directory_options::skip_permission_denied,
           ec),
       end;
       it != end && !ec; it.increment(ec)) {
    #ifdef _WIN32
    clearReadonlyAttribute(it->path());
    #endif
    std::error_code permEc;
    std::filesystem::permissions(it->path(), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add, permEc);
  }
}

bool removePath(const std::filesystem::path& absolutePath, std::string& error) {
  std::error_code ec;
  if (std::filesystem::is_directory(absolutePath, ec) && !ec) {
    (void)std::filesystem::remove_all(absolutePath, ec);
  } else {
    ec.clear();
    (void)std::filesystem::remove(absolutePath, ec);
  }
  if (!ec) {
    return true;
  }

  makeWritableRecursive(absolutePath);
  ec.clear();
  if (std::filesystem::is_directory(absolutePath, ec) && !ec) {
    (void)std::filesystem::remove_all(absolutePath, ec);
  } else {
    ec.clear();
    (void)std::filesystem::remove(absolutePath, ec);
  }
  if (!ec) {
    return true;
  }

  error = ec.message();
  return false;
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

    std::string removeError;
    if (!removePath(absolutePath, removeError)) {
      std::cout << "[ERROR] Failed to remove " << displayName << ": "
                << removeError << '\n';
      return false;
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

CMakeAdapter::CMakeAdapter(std::filesystem::path rootPath)
    : m_rootPath(std::move(rootPath)) {}

void CMakeAdapter::install(const ultra::cli::CommandOptions& options) {
  std::string command = "cmake -S . -B build -DCMAKE_BUILD_TYPE=" +
                        buildType(options);
  if (options.force) {
    command += " --fresh";
  }
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void CMakeAdapter::dev(const ultra::cli::CommandOptions& options) {
  ultra::cli::CommandOptions localOptions = options;
  if (!localOptions.release && !localOptions.debug) {
    localOptions.debug = true;
  }

  install(localOptions);
  if (m_lastExitCode != 0) {
    return;
  }

  build(localOptions);
}

void CMakeAdapter::build(const ultra::cli::CommandOptions& options) {
  if (options.clean) {
    clean(options);
    if (m_lastExitCode != 0) {
      return;
    }
  }

  if (!std::filesystem::exists(m_rootPath / "build")) {
    m_lastExitCode = execute("cmake -S . -B build", options);
    if (m_lastExitCode != 0) {
      return;
    }
  }

  std::string command = "cmake --build build";
  if (options.debug) {
    command += " --config Debug";
  } else {
    command += " --config Release";
  }
  if (options.parallel) {
    command += " --parallel";
  }
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }

  m_lastExitCode = execute(command, options);
}

void CMakeAdapter::test(const ultra::cli::CommandOptions& options) {
  std::string command = "ctest --test-dir build -C " + buildType(options) +
                        " --output-on-failure";
  if (options.parallel) {
    command += " --parallel 0";
  }
  if (!options.nativeArgs.empty()) {
    command += " ";
    command += options.nativeArgs;
  }
  m_lastExitCode = execute(command, options);
}

void CMakeAdapter::run(const ultra::cli::CommandOptions& options) {
  std::string command;
  if (!options.nativeArgs.empty()) {
    command = options.nativeArgs;
  } else {
    command = "cmake --build build --target run --config " + buildType(options);
  }
  m_lastExitCode = execute(command, options);
}

void CMakeAdapter::clean(const ultra::cli::CommandOptions& options) {
  bool removedAnything = false;

  if (!removeEntry(m_rootPath / "build", "build", options, removedAnything)) {
    m_lastExitCode = 1;
    return;
  }

  if (options.deep) {
    if (!removeEntry(m_rootPath / "CMakeCache.txt", "CMakeCache.txt", options,
                     removedAnything)) {
      m_lastExitCode = 1;
      return;
    }
    if (!removeEntry(m_rootPath / "CMakeFiles", "CMakeFiles", options,
                     removedAnything)) {
      m_lastExitCode = 1;
      return;
    }
  }

  if (!removedAnything) {
    std::cout << "[CLEAN] Nothing to clean.\n";
  }

  m_lastExitCode = 0;
}

int CMakeAdapter::lastExitCode() const noexcept {
  return m_lastExitCode;
}

bool CMakeAdapter::selfCheck(bool verbose) const {
  const bool cmakeInstalled = isToolAvailable("cmake");
  const bool cmakeListsPresent =
      std::filesystem::exists(m_rootPath / "CMakeLists.txt");

  if (verbose) {
    std::cout << "[doctor] cmake tool='cmake' "
              << (cmakeInstalled ? "OK" : "Missing") << '\n';
    std::cout << "[doctor] cmake file='CMakeLists.txt' "
              << (cmakeListsPresent ? "OK" : "Missing") << '\n';
  }

  return cmakeInstalled && cmakeListsPresent;
}

bool CMakeAdapter::ensureCMakeAvailable(
    const ultra::cli::CommandOptions& options) const {
  if (options.dryRun) {
    return true;
  }

  if (isToolAvailable("cmake")) {
    return true;
  }

  std::cout << "[ERROR] CMake not found in PATH.\n";
  std::cout << "Install from https://cmake.org\n";
  return false;
}

int CMakeAdapter::execute(const std::string& command,
                          const ultra::cli::CommandOptions& options) {
  if (!ensureCMakeAvailable(options)) {
    return 1;
  }
  return runCommand(m_rootPath, command, options);
}

std::string CMakeAdapter::buildType(const ultra::cli::CommandOptions& options) {
  if (options.debug) {
    return "Debug";
  }
  return "Release";
}

}  // namespace ultra::adapters
