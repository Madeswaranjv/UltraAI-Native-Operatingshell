#pragma once

#include "external/json.hpp"
//E:\Projects\Ultra\include\ultra\ipc\ultra_ipc_client.h
#include <chrono>
#include <filesystem>

namespace ultra::ipc {

class UltraIPCClient {
 public:
  using Json = nlohmann::json;

  explicit UltraIPCClient(
      std::filesystem::path projectRoot = std::filesystem::current_path());

  [[nodiscard]] Json sendRequest(
      const Json& request,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000))
      const;

  [[nodiscard]] bool isReachable(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) const;

  [[nodiscard]] const std::filesystem::path& projectRoot() const noexcept;
  [[nodiscard]] std::filesystem::path stateDirectory() const;
#ifndef _WIN32
  [[nodiscard]] std::filesystem::path socketPath() const;
#else
  [[nodiscard]] const std::wstring& pipeName() const noexcept;
#endif

 private:
  std::filesystem::path projectRoot_;
#ifndef _WIN32
  std::filesystem::path socketPath_;
#else
  std::wstring pipeName_;
#endif
};

}  // namespace ultra::ipc
