#pragma once

#include "ultra/ipc/ipc_transport.h"
//E:\Projects\Ultra\src\runtime\ipc_server.h
#include <external/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ultra::runtime {

struct IpcRequest {
  std::string id;
  std::string command;
  std::string type;
  nlohmann::json payload{nlohmann::json::object()};
};

struct IpcResponse {
  bool ok{false};
  int exitCode{1};
  std::string message;
  nlohmann::json payload;
};

struct DaemonHeartbeat {
  bool runtimeActive{false};
  std::uint64_t lastHeartbeat{0U};
  std::size_t indexedFiles{0U};
  std::size_t symbolsIndexed{0U};
  std::size_t dependenciesIndexed{0U};
  std::size_t graphNodes{0U};
  std::size_t graphEdges{0U};
  std::size_t pendingChanges{0U};
  std::size_t hotSliceSize{0U};
  std::size_t snapshotCount{0U};
  std::size_t activeBranchCount{0U};
};

enum class DaemonHealthState {
  Missing,
  Healthy,
  Stale,
  Dead,
  Corrupt,
};

struct DaemonHealthReport {
  DaemonHealthState state{DaemonHealthState::Missing};
  unsigned long pid{0UL};
  std::uint64_t startedAt{0U};
  std::uint64_t lastHeartbeat{0U};
  bool runtimeActive{false};
  std::size_t indexedFiles{0U};
  std::size_t symbolsIndexed{0U};
  std::size_t dependenciesIndexed{0U};
  std::size_t graphNodes{0U};
  std::size_t graphEdges{0U};
  std::size_t pendingChanges{0U};
  std::size_t memoryUsageBytes{0U};
  std::string message;

  [[nodiscard]] bool healthy() const noexcept {
    return state == DaemonHealthState::Healthy;
  }
};

class IpcServer {
 public:
  using RequestHandler = std::function<IpcResponse(const IpcRequest&)>;

  explicit IpcServer(std::filesystem::path projectRoot);

  bool start(std::string& error);
  void stop();
  bool writeHeartbeat(const DaemonHeartbeat& heartbeat, std::string& error) const;

  bool processRequests(const RequestHandler& handler,
                       std::size_t maxRequests,
                       std::string& error);

  static bool daemonLockPresent(const std::filesystem::path& projectRoot);
  static DaemonHealthReport inspectDaemon(
      const std::filesystem::path& projectRoot,
      std::chrono::milliseconds freshness = std::chrono::seconds(5));
  static bool waitForHealthy(
      const std::filesystem::path& projectRoot,
      std::chrono::milliseconds timeout,
      DaemonHealthReport& reportOut,
      std::string& error,
      std::chrono::milliseconds freshness = std::chrono::seconds(5));
  static bool cleanupStaleArtifacts(const std::filesystem::path& projectRoot,
                                    std::string& error);
  static bool terminateProcessForProject(
      const std::filesystem::path& projectRoot,
      std::string& error,
      std::chrono::milliseconds waitTimeout = std::chrono::seconds(2));
  static bool spawnDetached(const std::filesystem::path& projectRoot,
                            std::string& error);
  static unsigned long currentProcessId();
  static std::size_t currentProcessMemoryUsageBytes();

 private:
  bool writeJsonAtomically(const std::filesystem::path& outputPath,
                           const nlohmann::json& payload,
                           std::string& error) const;

  std::filesystem::path projectRoot_;
  std::filesystem::path runtimeDir_;
  std::filesystem::path ipcDir_;
  std::filesystem::path requestsDir_;
  std::filesystem::path responsesDir_;
  std::filesystem::path lockFile_;
  std::filesystem::path heartbeatFile_;
  std::filesystem::path socketFile_;
  std::unique_ptr<ultra::ipc::IpcTransport> transport_;
  bool started_{false};
};

}  // namespace ultra::runtime
