#include "AiRuntimeManager.h"

#include "../runtime/ipc_client.h"
#include "../runtime/ipc_server.h"
#include "ultra/runtime/daemon_runtime.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
//E:\Projects\Ultra\src\ai\AiRuntimeManager.cpp
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace ultra::ai {

namespace {

bool printStatusSnapshot(const nlohmann::json& payload, const bool verbose) {
  const bool runtimeActive = payload.value("runtime_active", false);
  std::cout << "AI runtime: " << (runtimeActive ? "active" : "inactive") << '\n';
  std::cout << "Daemon PID: " << payload.value("daemon_pid", 0UL) << '\n';
  std::cout << "Files indexed: " << payload.value("files_indexed", 0U) << '\n';
  std::cout << "Symbols indexed: " << payload.value("symbols_indexed", 0U)
            << '\n';
  std::cout << "Dependencies indexed: "
            << payload.value("dependencies_indexed", 0U) << '\n';
  std::cout << "Pending changes: " << payload.value("pending_changes", 0U)
            << '\n';
  std::cout << "Graph nodes: " << payload.value("graph_nodes", 0U) << '\n';
  std::cout << "Graph edges: " << payload.value("graph_edges", 0U) << '\n';
  std::cout << "Memory usage (bytes): "
            << payload.value("memory_usage_bytes", 0U) << '\n';
  std::cout << "Schema version: " << payload.value("schema_version", 0U)
            << '\n';
  std::cout << "Index version: " << payload.value("index_version", 0U) << '\n';

  if (verbose) {
    const nlohmann::json health =
        payload.value("kernel_health", nlohmann::json::object());
    std::cout << "Kernel healthy: "
              << (health.value("healthy", false) ? "yes" : "no") << '\n';
  }

  return runtimeActive;
}

}  // namespace

AiRuntimeManager::AiRuntimeManager(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()) {}

bool AiRuntimeManager::requestDaemon(const std::filesystem::path& projectRoot,
                                     const std::string& command,
                                     nlohmann::json& response,
                                     std::string& error) {
  return requestDaemon(projectRoot, command, nlohmann::json::object(), response,
                       error);
}

bool AiRuntimeManager::requestDaemon(const std::filesystem::path& projectRoot,
                                     const std::string& command,
                                     const nlohmann::json& requestPayload,
                                     nlohmann::json& response,
                                     std::string& error) {
  runtime::IpcClient ipcClient(projectRoot);
  return ipcClient.request(command, requestPayload, response, error);
}

int AiRuntimeManager::wakeAi(const bool verbose) {
  if (daemonChildModeEnabled()) {
    return runDaemonLoop(verbose);
  }

  const runtime::DaemonHealthReport health =
      runtime::IpcServer::inspectDaemon(projectRoot_);
  if (health.healthy()) {
    if (verbose) {
      std::cout << "[UAIR] daemon already running (pid=" << health.pid << ")\n";
    }
    return 0;
  }

  std::string error;
  if (health.state == runtime::DaemonHealthState::Stale) {
    if (!runtime::IpcServer::terminateProcessForProject(projectRoot_, error)) {
      if (verbose) {
        std::cerr << "[UAIR] failed to terminate stale daemon: " << error << '\n';
      }
      return 1;
    }
  } else if (health.state == runtime::DaemonHealthState::Dead ||
             health.state == runtime::DaemonHealthState::Corrupt) {
    if (!runtime::IpcServer::cleanupStaleArtifacts(projectRoot_, error)) {
      if (verbose) {
        std::cerr << "[UAIR] failed to cleanup stale daemon artifacts: " << error
                  << '\n';
      }
      return 1;
    }
  }

  if (!runtime::IpcServer::spawnDetached(projectRoot_, error)) {
    if (verbose) {
      std::cerr << "[UAIR] failed to spawn daemon: " << error << '\n';
    }
    return 1;
  }

  runtime::DaemonHealthReport ready;
  if (!runtime::IpcServer::waitForHealthy(projectRoot_, std::chrono::seconds(5),
                                          ready, error)) {
    (void)runtime::IpcServer::cleanupStaleArtifacts(projectRoot_, error);
    if (verbose) {
      std::cerr << "[UAIR] daemon failed to become healthy: " << error << '\n';
    }
    return 1;
  }

  if (verbose) {
    std::cout << "[UAIR] daemon_started pid=" << ready.pid << '\n';
  }
  return 0;
}

int AiRuntimeManager::runDaemonLoop(const bool verbose) {
  runtime::DaemonRuntime daemon(projectRoot_);
  runtime::DaemonRuntime::Options options;
  options.verbose = verbose;

  std::string error;
  const int code = daemon.run(options, error);
  if (code != 0 && verbose) {
    if (error.empty()) {
      std::cerr << "[UAIR] daemon exited with error: unknown startup/runtime failure\n";
    } else {
      std::cerr << "[UAIR] daemon exited with error: " << error << '\n';
    }
  }
  return code;
}

int AiRuntimeManager::rebuildAi(const bool verbose) {
  nlohmann::json response;
  std::string error;
  if (!requestDaemon(projectRoot_, "rebuild_ai", response, error)) {
    if (verbose) {
      std::cerr << "[UAIR] rebuild request failed: " << error << '\n';
    }
    return 1;
  }
  if (verbose) {
    std::cout << "[UAIR] " << response.value("message", "rebuild_enqueued")
              << '\n';
  }
  return response.value("exit_code", 0);
}

int AiRuntimeManager::aiStatus(const bool verbose) {
  nlohmann::json requestPayload = nlohmann::json::object();
  if (verbose) {
    requestPayload["verbose"] = true;
  }

  nlohmann::json response;
  std::string error;
  if (!requestDaemon(projectRoot_, "ai_status", requestPayload, response, error)) {
    if (verbose) {
      std::cerr << "[UAIR] status request failed: " << error << '\n';
    }
    return 1;
  }

  const nlohmann::json payload = response.value("payload", nlohmann::json::object());
  (void)printStatusSnapshot(payload, verbose);
  return response.value("exit_code", response.value("ok", false) ? 0 : 1);
}

int AiRuntimeManager::aiVerify(const bool verbose) {
  nlohmann::json requestPayload;
  requestPayload["verbose"] = true;

  nlohmann::json response;
  std::string error;
  if (!requestDaemon(projectRoot_, "ai_status", requestPayload, response, error)) {
    if (verbose) {
      std::cerr << "[UAIR] verify failed: " << error << '\n';
    }
    return 1;
  }

  const nlohmann::json payload = response.value("payload", nlohmann::json::object());
  const nlohmann::json health = payload.value("kernel_health", nlohmann::json::object());
  const bool healthy = health.value("healthy", false);
  if (verbose) {
    std::cout << "Kernel healthy: " << (healthy ? "yes" : "no") << '\n';
  }
  return healthy ? 0 : 1;
}

bool AiRuntimeManager::contextDiff(nlohmann::json& payloadOut, std::string& error) {
  if (runtime::IpcServer::inspectDaemon(projectRoot_).healthy()) {
    nlohmann::json response;
    if (!requestDaemon(projectRoot_, "context_diff", response, error)) {
      return false;
    }
    payloadOut = response.value("payload", nlohmann::json::object());
    return true;
  }

  runtime::DaemonRuntime runtime(projectRoot_);
  runtime::DaemonRuntime::Options options;
  options.verbose = false;
  if (!runtime.start(options, error)) {
    return false;
  }

  const bool ok = runtime.contextDiff(payloadOut, error);
  runtime.stop();
  return ok;
}

void AiRuntimeManager::silentIncrementalUpdate() {
  nlohmann::json response;
  std::string error;
  nlohmann::json payload;
  payload["verbose"] = false;
  (void)requestDaemon(projectRoot_, "ai_status", payload, response, error);
}

bool AiRuntimeManager::daemonChildModeEnabled() {
#ifdef _WIN32
  int argc = 0;
  LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (argv != nullptr) {
    for (int index = 0; index < argc; ++index) {
      if (argv[index] != nullptr &&
          std::wstring(argv[index]) == L"--uair-child") {
        ::LocalFree(argv);
        return true;
      }
    }
    ::LocalFree(argv);
  }

  char* buffer = nullptr;
  size_t len = 0U;
  if (_dupenv_s(&buffer, &len, "ULTRA_UAIR_CHILD") != 0 || buffer == nullptr) {
    return false;
  }
  const std::string value(buffer);
  std::free(buffer);
  return value == "1" || value == "true";
#else
  const char* value = std::getenv("ULTRA_UAIR_CHILD");
  return value != nullptr &&
         (std::string(value) == "1" || std::string(value) == "true");
#endif
}

}  // namespace ultra::ai
