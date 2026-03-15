#include "UniversalCLI.h"
#include "CommandOptionsParser.h"
#include "../ai/AiRuntimeManager.h"
#include "../adapters/AdapterCommandRunner.h"
#include "../adapters/AdapterFactory.h"
#include "../adapters/CMakeAdapter.h"
#include "../adapters/ReactAdapter.h"
#include "../core/ProjectTypeDetector.h"
#include <algorithm>
#include <cctype>
#include <iostream>
//E:\Projects\Ultra\src\cli\UniversalCLI.cpp
namespace ultra::cli {

namespace {

std::string trim(const std::string& value) {
  const std::size_t first = value.find_first_not_of(" \t\n\r");
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = value.find_last_not_of(" \t\n\r");
  return value.substr(first, last - first + 1);
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool hasUnsupportedDoctorFlags(const CommandOptions& options) {
  return options.release || options.debug || options.watch || options.parallel ||
         options.force || options.clean || options.dryRun ||
         !options.nativeArgs.empty();
}

bool shouldRunAiIncremental(const std::string& command) {
  return command == "build" || command == "dev" || command == "run" ||
         command == "test";
}

void printDoctorKernelHealth(const nlohmann::json& health) {
  if (!health.is_object()) {
    return;
  }

  std::cout << "Kernel Health: "
            << (health.value("healthy", false) ? "OK" : "Issue") << '\n';
  std::cout << "Branch count: " << health.value("branch_count", 0U) << '\n';
  std::cout << "Snapshot count: " << health.value("snapshot_count", 0U) << '\n';
  std::cout << "Governance active: "
            << (health.value("governance_active", false) ? "yes" : "no")
            << '\n';
  std::cout << "Determinism guards: "
            << (health.value("determinism_guards_active", false) ? "yes"
                                                                  : "no")
            << '\n';
  std::cout << "Memory caps respected: "
            << (health.value("memory_caps_respected", false) ? "yes" : "no")
            << '\n';

  const nlohmann::json violations =
      health.value("violations", nlohmann::json::array());
  if (violations.empty()) {
    return;
  }

  std::cout << "Kernel violations:\n";
  for (const auto& item : violations) {
    if (!item.is_string()) {
      continue;
    }
    std::cout << "  - " << item.get<std::string>() << '\n';
  }
}

}  // namespace

int UniversalCLI::execute(const std::string& command,
                          const std::vector<std::string>& rawArgs) {
  const std::filesystem::path rootPath = std::filesystem::current_path();

  if (command == "exec") {
    return executeExec(rawArgs, rootPath);
  }

  if (command == "doctor") {
    return executeDoctor(rawArgs);
  }

  if (command == "init") {
    return m_initCommand.execute(rawArgs);
  }

  const CommandOptionsParseResult parseResult =
      CommandOptionsParser::parse(rawArgs, false);
  if (!parseResult.ok) {
    std::cout << "[ERROR] " << parseResult.error << '\n';
    return 1;
  }

  const CommandOptions options = parseResult.options;

  ultra::config::ConfigLoader configLoader(rootPath);
  if (!configLoader.load()) {
    std::cout << "[ERROR] " << configLoader.lastError() << '\n';
    return 1;
  }

  if (configLoader.hasConfig() && !configLoader.config().modules.empty()) {
    return executeForModules(command, options, configLoader.config());
  }

  const ultra::core::ProjectType detectedType =
      ultra::core::ProjectTypeDetector::detect(rootPath);
  return executeForTarget(command, options, rootPath, detectedType, "root");
}

bool UniversalCLI::isUniversalCommand(const std::string& command) {
  return command == "init" || command == "install" || command == "dev" ||
         command == "build" || command == "test" || command == "run" ||
         command == "clean" || command == "exec" || command == "doctor";
}

int UniversalCLI::executeForTarget(const std::string& command,
                                   const CommandOptions& options,
                                   const std::filesystem::path& rootPath,
                                   ultra::core::ProjectType projectType,
                                   const std::string& targetLabel) {
  if (!std::filesystem::exists(rootPath) ||
      !std::filesystem::is_directory(rootPath)) {
    std::cout << "[ERROR] Invalid module path for '" << targetLabel
              << "': " << rootPath.string() << '\n';
    return 1;
  }

  if (projectType == ultra::core::ProjectType::Unknown) {
    std::cout << "[ERROR] Could not detect project type in current directory.\n";
    std::cout << "Supported stacks:\n";
    std::cout << "    - React (package.json)\n";
    std::cout << "    - CMake (CMakeLists.txt)\n";
    std::cout << "    - Rust (Cargo.toml)\n";
    std::cout << "    - Django (manage.py)\n";
    return 1;
  }

  std::cout << "[universal] target='" << targetLabel << "' type='"
            << ultra::core::ProjectTypeDetector::toString(projectType) << "'\n";

  std::unique_ptr<ultra::adapters::ProjectAdapter> adapter =
      ultra::adapters::createProjectAdapter(projectType, rootPath);
  if (shouldRunAiIncremental(command)) {
    ultra::ai::AiRuntimeManager runtime(rootPath);
    runtime.silentIncrementalUpdate();
  }
  dispatchOnAdapter(command, *adapter, options);
  return adapter->lastExitCode();
}

int UniversalCLI::executeForModules(const std::string& command,
                                    const CommandOptions& options,
                                    const ultra::config::UltraConfig& config) {
  for (const ultra::config::ModuleConfig& module : config.modules) {
    const int code =
        executeForTarget(command, options, module.path, module.type, module.name);
    if (code != 0) {
      return code;
    }
  }
  return 0;
}

int UniversalCLI::executeExec(const std::vector<std::string>& rawArgs,
                              const std::filesystem::path& rootPath) {
  CommandOptions options;
  std::size_t commandStart = 0;

  while (commandStart < rawArgs.size()) {
    const std::string& token = rawArgs[commandStart];
    const std::string normalized = toLower(token);
    if (normalized == "--verbose") {
      options.verbose = true;
      ++commandStart;
      continue;
    }
    if (normalized == "--dry-run") {
      options.dryRun = true;
      ++commandStart;
      continue;
    }
    if (token == "--") {
      ++commandStart;
      break;
    }
    break;
  }

  if (commandStart >= rawArgs.size()) {
    std::cout << "[ERROR] Command 'exec' requires a native command to forward.\n";
    return 1;
  }

  options.nativeArgs = CommandOptionsParser::joinForShell(rawArgs, commandStart);
  options.nativeArgs = trim(options.nativeArgs);
  if (options.nativeArgs.empty()) {
    std::cout << "[ERROR] Command 'exec' requires a non-empty native command.\n";
    return 1;
  }

  ultra::config::ConfigLoader configLoader(rootPath);
  if (!configLoader.load()) {
    std::cout << "[ERROR] " << configLoader.lastError() << '\n';
    return 1;
  }

  if (configLoader.hasConfig() && !configLoader.config().modules.empty()) {
    for (const ultra::config::ModuleConfig& module : configLoader.config().modules) {
      if (!std::filesystem::exists(module.path) ||
          !std::filesystem::is_directory(module.path)) {
        std::cout << "[ERROR] Invalid module path for '" << module.name
                  << "': " << module.path.string() << '\n';
        return 1;
      }
      const int code =
          ultra::adapters::runCommand(module.path, options.nativeArgs, options);
      if (code != 0) {
        return code;
      }
    }
    return 0;
  }

  return ultra::adapters::runCommand(rootPath, options.nativeArgs, options);
}

int UniversalCLI::executeDoctor(const std::vector<std::string>& rawArgs) {
  const CommandOptionsParseResult parseResult =
      CommandOptionsParser::parse(rawArgs, false);
  if (!parseResult.ok) {
    std::cout << "[ERROR] " << parseResult.error << '\n';
    return 0;
  }

  if (hasUnsupportedDoctorFlags(parseResult.options)) {
    std::cout << "[ERROR] Command 'doctor' supports only --deep and --verbose.\n";
    return 0;
  }

  const CommandOptions options = parseResult.options;
  const bool nodeInstalled = ultra::adapters::isToolAvailable("node");
  const bool cmakeInstalled = ultra::adapters::isToolAvailable("cmake");
  const bool cargoInstalled = ultra::adapters::isToolAvailable("cargo");
  const bool pythonInstalled = ultra::adapters::isToolAvailable("python");
  const bool djangoInstalled = ultra::adapters::isToolAvailable("django-admin");

  std::cout << "Node: " << (nodeInstalled ? "Installed" : "Not Found") << '\n';
  std::cout << "CMake: " << (cmakeInstalled ? "Installed" : "Not Found")
            << '\n';
  std::cout << "Cargo: " << (cargoInstalled ? "Installed" : "Not Found")
            << '\n';
  std::cout << "Python: " << (pythonInstalled ? "Installed" : "Not Found")
            << '\n';
  std::cout << "Django: " << (djangoInstalled ? "Installed" : "Not Found")
            << '\n';

  const std::filesystem::path rootPath = std::filesystem::current_path();
  nlohmann::json daemonResponse;
  std::string daemonError;
  nlohmann::json daemonPayload = nlohmann::json::object();
  daemonPayload["verbose"] = true;
  if (ultra::ai::AiRuntimeManager::requestDaemon(rootPath, "ai_status",
                                                 daemonPayload, daemonResponse,
                                                 daemonError)) {
    const nlohmann::json statusPayload =
        daemonResponse.value("payload", nlohmann::json::object());
    printDoctorKernelHealth(
        statusPayload.value("kernel_health", nlohmann::json::object()));
  } else if (options.verbose) {
    std::cout << "[doctor] Kernel health unavailable: " << daemonError << '\n';
  }

  if (!options.deep) {
    return 0;
  }

  bool ranCheck = false;
  bool allChecksPassed = true;

  const auto runSelfCheck =
      [&](ultra::core::ProjectType type,
          const std::filesystem::path& path,
          const std::string& label) {
        if (type == ultra::core::ProjectType::React) {
          const ultra::adapters::ReactAdapter adapter(path);
          const bool ok = adapter.selfCheck(options.verbose);
          std::cout << "React Adapter (" << label << "): "
                    << (ok ? "OK" : "Issue") << '\n';
          ranCheck = true;
          allChecksPassed = allChecksPassed && ok;
          return;
        }
        if (type == ultra::core::ProjectType::CMake) {
          const ultra::adapters::CMakeAdapter adapter(path);
          const bool ok = adapter.selfCheck(options.verbose);
          std::cout << "CMake Adapter (" << label << "): "
                    << (ok ? "OK" : "Issue") << '\n';
          ranCheck = true;
          allChecksPassed = allChecksPassed && ok;
          return;
        }
        if (options.verbose) {
          std::cout << "[doctor] Skipping deep self-check for type '"
                    << ultra::core::ProjectTypeDetector::toString(type)
                    << "'\n";
        }
      };

  ultra::config::ConfigLoader configLoader(rootPath);
  if (!configLoader.load()) {
    std::cout << "[ERROR] " << configLoader.lastError() << '\n';
    return 0;
  }

  if (configLoader.hasConfig() && !configLoader.config().modules.empty()) {
    for (const ultra::config::ModuleConfig& module : configLoader.config().modules) {
      runSelfCheck(module.type, module.path, module.name);
    }
  } else {
    const ultra::core::ProjectType detectedType =
        ultra::core::ProjectTypeDetector::detect(rootPath);
    runSelfCheck(detectedType, rootPath, "root");
  }

  if (!ranCheck) {
    std::cout << "[doctor] No deep adapter self-check available for detected stack.\n";
    return 0;
  }

  if (!allChecksPassed) {
    std::cout << "[ERROR] One or more deep adapter checks failed.\n";
    return 0;
  }

  return 0;
}

void UniversalCLI::dispatchOnAdapter(const std::string& command,
                                     ultra::adapters::ProjectAdapter& adapter,
                                     const CommandOptions& options) {
  if (command == "install") {
    adapter.install(options);
    return;
  }
  if (command == "dev") {
    adapter.dev(options);
    return;
  }
  if (command == "build") {
    adapter.build(options);
    return;
  }
  if (command == "test") {
    adapter.test(options);
    return;
  }
  if (command == "run") {
    adapter.run(options);
    return;
  }
  if (command == "clean") {
    adapter.clean(options);
    return;
  }

  std::cout << "[ERROR] Unsupported universal command: " << command << '\n';
}

}  // namespace ultra::cli
