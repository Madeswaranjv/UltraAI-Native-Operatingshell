#pragma once

#include "ultra/ipc/ipc_transport.h"
//E:\Projects\Ultra\src\runtime\ipc_client.h
#include <external/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace ultra::runtime {

class IpcClient {
 public:
  explicit IpcClient(std::filesystem::path projectRoot);

  [[nodiscard]] bool isDaemonRunning() const;
  bool request(const std::string& command,
               const nlohmann::json& requestPayload,
               nlohmann::json& responseOut,
               std::string& error,
               std::chrono::milliseconds timeout = std::chrono::seconds(15))
      const;
  bool request(const std::string& command,
               nlohmann::json& responseOut,
               std::string& error,
               std::chrono::milliseconds timeout = std::chrono::seconds(15))
      const;

 private:
  std::filesystem::path projectRoot_;
  std::filesystem::path ipcDir_;
  std::filesystem::path requestsDir_;
  std::filesystem::path responsesDir_;
  std::filesystem::path lockFile_;
  mutable std::unique_ptr<ultra::ipc::IpcTransport> transport_;
};

}  // namespace ultra::runtime
