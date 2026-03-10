#pragma once

#include <external/json.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace ultra::ipc {

struct TransportMessage {
  std::string id;
  std::string command;
  nlohmann::json payload{nlohmann::json::object()};
};

class IpcTransport {
 public:
  virtual ~IpcTransport() = default;

  virtual bool start(std::string& error) = 0;
  virtual void stop() = 0;

  virtual bool send(const TransportMessage& message,
                    std::chrono::milliseconds timeout,
                    nlohmann::json& responseOut,
                    std::string& error) = 0;
  virtual bool receive(TransportMessage& messageOut,
                       std::chrono::milliseconds timeout,
                       std::string& error) = 0;
  virtual bool respond(const std::string& requestId,
                       const nlohmann::json& response,
                       std::string& error) = 0;
};

std::unique_ptr<IpcTransport> makeUdsTransport(std::filesystem::path projectRoot);
std::unique_ptr<IpcTransport> makeNamedPipeTransport(
    std::filesystem::path projectRoot);
std::unique_ptr<IpcTransport> makePlatformTransport(
    std::filesystem::path projectRoot);

}  // namespace ultra::ipc
