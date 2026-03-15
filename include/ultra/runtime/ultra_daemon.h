#pragma once

#include "external/json.hpp"
#include "ultra/ipc/ultra_ipc_server.h"
//E:\Projects\Ultra\include\ultra\runtime\ultra_daemon.h
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ultra::runtime {

class UltraDaemon {
 public:
  using Json = nlohmann::json;
  using RuntimeRequestHandler =
      std::function<Json(const std::string& type, const Json& payload)>;

  explicit UltraDaemon(
      std::filesystem::path projectRoot = std::filesystem::current_path());

  UltraDaemon(const UltraDaemon&) = delete;
  UltraDaemon& operator=(const UltraDaemon&) = delete;
  UltraDaemon(UltraDaemon&&) = delete;
  UltraDaemon& operator=(UltraDaemon&&) = delete;

  bool run(const RuntimeRequestHandler& runtimeHandler);
  void requestStop() noexcept;

  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] const std::filesystem::path& projectRoot() const noexcept;

  static bool wake(const std::filesystem::path& projectRoot,
                   const std::filesystem::path& executablePath,
                   const std::vector<std::string>& daemonArgs =
                       std::vector<std::string>{"--ultra-daemon"});
  static bool sleep(const std::filesystem::path& projectRoot,
                    std::chrono::milliseconds timeout =
                        std::chrono::milliseconds(5000));
  static bool isDaemonAlive(const std::filesystem::path& projectRoot);

  [[nodiscard]] static std::filesystem::path daemonStateDirectory(
      const std::filesystem::path& projectRoot);
  [[nodiscard]] static std::filesystem::path daemonPidFile(
      const std::filesystem::path& projectRoot);

 private:
  [[nodiscard]] Json handleRequest(const Json& request,
                                   const RuntimeRequestHandler& runtimeHandler);
  bool writePidFile() const;
  void cleanupState() const;

  static bool spawnDetached(const std::filesystem::path& executablePath,
                            const std::vector<std::string>& daemonArgs,
                            const std::filesystem::path& projectRoot);

  std::filesystem::path projectRoot_;
  std::atomic<bool> running_{false};
  ultra::ipc::UltraIPCServer ipcServer_;
};

}  // namespace ultra::runtime
