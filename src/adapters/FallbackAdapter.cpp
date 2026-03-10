#include "FallbackAdapter.h"
#include "AdapterCommandRunner.h"
#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>
#include <utility>

namespace ultra::adapters {

namespace {

std::string displayPath(const std::filesystem::path& root,
                        const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::path relative = std::filesystem::relative(path, root, ec);
  std::string label = ec ? path.filename().string() : relative.generic_string();
  if (label.empty()) {
    label = path.filename().string();
  }
  return label;
}

bool removeEntry(const std::filesystem::path& root,
                 const std::filesystem::path& path,
                 const ultra::cli::CommandOptions& options,
                 bool& removedAnything) {
  try {
    if (!std::filesystem::exists(path)) {
      return true;
    }

    const bool isDirectory = std::filesystem::is_directory(path);
    const std::string label = displayPath(root, path);

    if (options.dryRun) {
      std::cout << "[CLEAN] Would remove " << label << '\n';
      removedAnything = true;
      return true;
    }

    if (isDirectory) {
      std::filesystem::remove_all(path);
    } else {
      std::filesystem::remove(path);
    }

    std::cout << "[CLEAN] Removed " << label << '\n';
    removedAnything = true;
    return true;
  } catch (const std::exception& ex) {
    std::cout << "[ERROR] Failed to clean path: " << path.string() << " ("
              << ex.what() << ")\n";
    return false;
  } catch (...) {
    std::cout << "[ERROR] Failed to clean path: " << path.string() << '\n';
    return false;
  }
}

}  // namespace

FallbackAdapter::FallbackAdapter(std::filesystem::path rootPath,
                                 ultra::core::ProjectType projectType)
    : m_rootPath(std::move(rootPath)), m_projectType(projectType) {}

void FallbackAdapter::install(const ultra::cli::CommandOptions& options) {
  executeAction(Action::Install, options);
}

void FallbackAdapter::dev(const ultra::cli::CommandOptions& options) {
  executeAction(Action::Dev, options);
}

void FallbackAdapter::build(const ultra::cli::CommandOptions& options) {
  executeAction(Action::Build, options);
}

void FallbackAdapter::test(const ultra::cli::CommandOptions& options) {
  executeAction(Action::Test, options);
}

void FallbackAdapter::run(const ultra::cli::CommandOptions& options) {
  executeAction(Action::Run, options);
}

void FallbackAdapter::clean(const ultra::cli::CommandOptions& options) {
  executeAction(Action::Clean, options);
}

int FallbackAdapter::lastExitCode() const noexcept {
  return m_lastExitCode;
}

void FallbackAdapter::executeAction(Action action,
                                    const ultra::cli::CommandOptions& options) {
  if (m_projectType == ultra::core::ProjectType::Python && action == Action::Clean) {
    cleanPythonProject(options);
    return;
  }

  if (m_projectType == ultra::core::ProjectType::Python) {
    if (isDjangoProject()) {
      if (!ensureDjangoTooling(options)) {
        m_lastExitCode = 1;
        return;
      }
    } else if (!ensurePythonAvailable(options)) {
      m_lastExitCode = 1;
      return;
    }
  }

  const std::string command = commandFor(action, options);
  if (command.empty()) {
    std::cout << "[ERROR] No adapter command available for detected project type.\n";
    m_lastExitCode = 1;
    return;
  }

  m_lastExitCode = runCommand(m_rootPath, command, options);
}

void FallbackAdapter::cleanPythonProject(const ultra::cli::CommandOptions& options) {
  bool removedAnything = false;

  try {
    std::vector<std::filesystem::path> pycFiles;
    std::vector<std::filesystem::path> pycacheDirs;

    std::filesystem::directory_options opts =
        std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(m_rootPath, opts), end;
         it != end; ++it) {
      std::error_code ec;
      const std::filesystem::path entryPath = it->path();

      if (it->is_directory(ec)) {
        if (entryPath.filename() == "__pycache__") {
          pycacheDirs.push_back(entryPath);
          it.disable_recursion_pending();
        }
        continue;
      }

      if (it->is_regular_file(ec) && entryPath.extension() == ".pyc") {
        pycFiles.push_back(entryPath);
      }
    }

    for (const auto& file : pycFiles) {
      if (!removeEntry(m_rootPath, file, options, removedAnything)) {
        m_lastExitCode = 1;
        return;
      }
    }

    std::sort(pycacheDirs.begin(), pycacheDirs.end(),
              [](const std::filesystem::path& lhs,
                 const std::filesystem::path& rhs) {
                return lhs.generic_string().size() > rhs.generic_string().size();
              });

    for (const auto& dir : pycacheDirs) {
      if (!removeEntry(m_rootPath, dir, options, removedAnything)) {
        m_lastExitCode = 1;
        return;
      }
    }

    if (options.deep && isDjangoProject()) {
      if (!removeEntry(m_rootPath, m_rootPath / "db.sqlite3", options,
                       removedAnything)) {
        m_lastExitCode = 1;
        return;
      }
    }
  } catch (const std::exception& ex) {
    std::cout << "[ERROR] Clean failed: " << ex.what() << '\n';
    m_lastExitCode = 1;
    return;
  } catch (...) {
    std::cout << "[ERROR] Clean failed.\n";
    m_lastExitCode = 1;
    return;
  }

  if (!removedAnything) {
    std::cout << "[CLEAN] Nothing to clean.\n";
  }

  m_lastExitCode = 0;
}

std::string FallbackAdapter::commandFor(
    Action action,
    const ultra::cli::CommandOptions& options) const {
  std::string command;

  if (m_projectType == ultra::core::ProjectType::Make) {
    switch (action) {
      case Action::Install:
        command = "make install";
        break;
      case Action::Dev:
        command = "make";
        break;
      case Action::Build:
        command = "make";
        break;
      case Action::Test:
        command = "make test";
        break;
      case Action::Run:
        command = "make run";
        break;
      case Action::Clean:
        command = "make clean";
        break;
    }

    if (options.parallel) {
      command += " -j";
    }
    if (options.release) {
      command += " MODE=release";
    }
    if (options.debug) {
      command += " MODE=debug";
    }
    if (!options.nativeArgs.empty()) {
      command += " ";
      command += options.nativeArgs;
    }
    return command;
  }

  if (m_projectType == ultra::core::ProjectType::Python) {
    if (isDjangoProject()) {
      switch (action) {
        case Action::Install:
          command = "python -m pip install -r requirements.txt";
          break;
        case Action::Dev:
          command = "python manage.py runserver";
          break;
        case Action::Build:
          command = "python manage.py check";
          break;
        case Action::Test:
          command = "python manage.py test";
          break;
        case Action::Run:
          command = options.nativeArgs.empty() ? "python manage.py runserver"
                                               : "python manage.py " + options.nativeArgs;
          return command;
        case Action::Clean:
          return {};
      }
    } else {
      switch (action) {
        case Action::Install:
          command = "python -m pip install -r requirements.txt";
          break;
        case Action::Dev:
          command = "python -m pip install -e .";
          break;
        case Action::Build:
          command = "python -m build";
          break;
        case Action::Test:
          command = "python -m pytest";
          break;
        case Action::Run:
          command = options.nativeArgs.empty() ? "python main.py"
                                               : "python " + options.nativeArgs;
          return command;
        case Action::Clean:
          return {};
      }
    }

    if (!options.nativeArgs.empty()) {
      command += " ";
      command += options.nativeArgs;
    }
    return command;
  }

  return {};
}

bool FallbackAdapter::ensurePythonAvailable(
    const ultra::cli::CommandOptions& options) const {
  if (options.dryRun) {
    return true;
  }

  if (isToolAvailable("python")) {
    return true;
  }

  std::cout << "[ERROR] Python not found in PATH.\n";
  return false;
}

bool FallbackAdapter::ensureDjangoTooling(
    const ultra::cli::CommandOptions& options) const {
  if (options.dryRun) {
    return true;
  }

  if (!isToolAvailable("python")) {
    std::cout << "[ERROR] Python not found in PATH.\n";
    return false;
  }

  if (isToolAvailable("django-admin")) {
    return true;
  }

  std::cout << "[ERROR] django-admin not found.\n";
  std::cout << "Run: pip install django\n";
  return false;
}

bool FallbackAdapter::isDjangoProject() const {
  return std::filesystem::exists(m_rootPath / "manage.py");
}

}  // namespace ultra::adapters
