#pragma once

#include <external/json.hpp>
#include<thread>
#include<stop_token>
#include <filesystem>
#include <memory>
#include <string>
//deamon_runtime.h
namespace ultra::runtime {

struct IpcRequest;
struct IpcResponse;

class DaemonRuntime {
 public:
  struct Options {
    bool verbose{false};
  };

  explicit DaemonRuntime(std::filesystem::path projectRoot);
  ~DaemonRuntime();

  DaemonRuntime(const DaemonRuntime&) = delete;
  DaemonRuntime& operator=(const DaemonRuntime&) = delete;
  DaemonRuntime(DaemonRuntime&&) noexcept;
  DaemonRuntime& operator=(DaemonRuntime&&) noexcept;

  bool start(const Options& options, std::string& error);
  int run(const Options& options, std::string& error);
  void requestStop();
  void stop();

  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] IpcResponse dispatchRequest(const IpcRequest& request);
  [[nodiscard]] nlohmann::json statusPayload(bool verboseStatus) const;
  bool contextDiff(nlohmann::json& payloadOut, std::string& error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ultra::runtime

