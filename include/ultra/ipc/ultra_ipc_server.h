#pragma once

#include "external/json.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
//E:\Projects\Ultra\include\ultra\ipc\ultra_ipc_server.h
namespace ultra::ipc {

class UltraIPCServer {
 public:
  using Json = nlohmann::json;
  using RequestHandler = std::function<Json(const Json&)>;

  explicit UltraIPCServer(
      std::filesystem::path projectRoot = std::filesystem::current_path());
  ~UltraIPCServer();

  UltraIPCServer(const UltraIPCServer&) = delete;
  UltraIPCServer& operator=(const UltraIPCServer&) = delete;
  UltraIPCServer(UltraIPCServer&&) = delete;
  UltraIPCServer& operator=(UltraIPCServer&&) = delete;

  bool start();
  void stop();
  bool processNextRequest(
      const RequestHandler& handler,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(250));

  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] const std::filesystem::path& projectRoot() const noexcept;
  [[nodiscard]] std::filesystem::path stateDirectory() const;
#ifndef _WIN32
  [[nodiscard]] std::filesystem::path socketPath() const;
#else
  [[nodiscard]] const std::wstring& pipeName() const noexcept;
#endif

 private:
  std::filesystem::path projectRoot_;
  bool running_{false};

#ifndef _WIN32
  std::filesystem::path socketPath_;
  int listenFd_{-1};
#else
  std::wstring pipeName_;
#endif
};

}  // namespace ultra::ipc
