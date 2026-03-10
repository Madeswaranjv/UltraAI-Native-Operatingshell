#include "ultra/ipc/ipc_transport.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <thread>
#include <utility>
#include <vector>
//E:\Projects\Ultra\src\ipc\ipc_transport_named_pipe.cpp
namespace ultra::ipc {

namespace {

class NamedPipeTransport final : public IpcTransport {
 public:
  explicit NamedPipeTransport(std::filesystem::path projectRoot)
      : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                         .lexically_normal()),
        runtimeDir_(projectRoot_ / ".ultra" / "runtime"),
        ipcDir_(runtimeDir_ / "ipc"),
        requestsDir_(ipcDir_ / "requests"),
        responsesDir_(ipcDir_ / "responses"),
        pipeMarker_(runtimeDir_ / "daemon.pipe") {}

  bool start(std::string& error) override {
    try {
      std::filesystem::create_directories(requestsDir_);
      std::filesystem::create_directories(responsesDir_);
      std::ofstream pipeOut(pipeMarker_, std::ios::binary | std::ios::trunc);
      if (!pipeOut) {
        error = "Failed to create daemon pipe marker: " + pipeMarker_.string();
        return false;
      }
      pipeOut << "named-pipe-fallback-transport";
      started_ = true;
      return true;
    } catch (const std::exception& ex) {
      error = std::string("Failed to initialize named pipe transport: ") +
              ex.what();
      return false;
    }
  }

  void stop() override {
    started_ = false;
  }

  bool send(const TransportMessage& message,
            const std::chrono::milliseconds timeout,
            nlohmann::json& responseOut,
            std::string& error) override {
    if (!started_ && !start(error)) {
      return false;
    }
    if (message.id.empty()) {
      error = "IPC transport message id is empty.";
      return false;
    }

    const std::filesystem::path requestPath = requestsDir_ / (message.id + ".json");
    const std::filesystem::path responsePath =
        responsesDir_ / (message.id + ".json");
    nlohmann::json payload;
    payload["id"] = message.id;
    payload["command"] = message.command;
    payload["type"] = message.command;
    payload["payload"] = message.payload;

    if (!writeJsonAtomically(requestPath, payload, error)) {
      return false;
    }

    std::string transientError;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() <= deadline) {
      std::error_code existsEc;
      const bool responseExists = std::filesystem::exists(responsePath, existsEc);
      if (existsEc) {
        transientError =
            "IPC response readiness check failed: " + existsEc.message();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      if (responseExists) {
        try {
          {
            std::ifstream input(responsePath, std::ios::binary);
            if (!input) {
              transientError = "Failed to open IPC response: " + responsePath.string();
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
              continue;
            }
            input >> responseOut;
          }
          std::error_code ec;
          std::filesystem::remove(responsePath, ec);
          return true;
        } catch (const std::exception& ex) {
          transientError = std::string("Failed to parse IPC response: ") + ex.what();
          std::error_code ec;
          std::filesystem::remove(responsePath, ec);
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!transientError.empty()) {
      error = "Timed out waiting for IPC response. Last error: " + transientError;
    } else {
      error = "Timed out waiting for IPC response.";
    }
    return false;
  }

  bool receive(TransportMessage& messageOut,
               const std::chrono::milliseconds timeout,
               std::string& error) override {
    if (!started_ && !start(error)) {
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() <= deadline) {
      std::vector<std::filesystem::path> requests;
      try {
        for (const auto& entry : std::filesystem::directory_iterator(requestsDir_)) {
          if (!entry.is_regular_file()) {
            continue;
          }
          if (entry.path().extension() != ".json") {
            continue;
          }
          requests.push_back(entry.path());
        }
      } catch (const std::exception& ex) {
        error = std::string("Failed to enumerate IPC requests: ") + ex.what();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      if (!requests.empty()) {
        std::sort(requests.begin(), requests.end());
        const std::filesystem::path requestPath = requests.front();
        try {
          nlohmann::json requestJson;
          {
            std::ifstream input(requestPath, std::ios::binary);
            if (!input) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
              continue;
            }
            input >> requestJson;
          }
          messageOut.id = requestJson.value("id", requestPath.stem().string());
          messageOut.command = requestJson.value("command", "");
          messageOut.payload =
              requestJson.value("payload", nlohmann::json::object());
          if (messageOut.payload.is_null()) {
            messageOut.payload = nlohmann::json::object();
          }
          std::error_code ec;
          std::filesystem::remove(requestPath, ec);
          return true;
        } catch (const std::exception& ex) {
          error = std::string("Failed to parse IPC request: ") + ex.what();
          std::error_code ec;
          std::filesystem::remove(requestPath, ec);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    messageOut = TransportMessage{};
    return true;
  }

  bool respond(const std::string& requestId,
               const nlohmann::json& response,
               std::string& error) override {
    if (!started_ && !start(error)) {
      return false;
    }
    if (requestId.empty()) {
      error = "IPC response request id is empty.";
      return false;
    }
    const std::filesystem::path responsePath = responsesDir_ / (requestId + ".json");
    return writeJsonAtomically(responsePath, response, error);
  }

 private:
  bool writeJsonAtomically(const std::filesystem::path& outputPath,
                           const nlohmann::json& payload,
                           std::string& error) const {
    try {
      const std::filesystem::path tmpPath = outputPath.string() + ".tmp";
      {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
          error = "Failed to open temporary IPC file: " + tmpPath.string();
          return false;
        }
        out << payload.dump(2);
        out.flush();
        if (!out) {
          error = "Failed to write temporary IPC file: " + tmpPath.string();
          return false;
        }
      }

      std::error_code ec;
      std::filesystem::remove(outputPath, ec);
      std::filesystem::rename(tmpPath, outputPath);
      return true;
    } catch (const std::exception& ex) {
      error = std::string("Failed to persist IPC payload: ") + ex.what();
      return false;
    }
  }

  std::filesystem::path projectRoot_;
  std::filesystem::path runtimeDir_;
  std::filesystem::path ipcDir_;
  std::filesystem::path requestsDir_;
  std::filesystem::path responsesDir_;
  std::filesystem::path pipeMarker_;
  bool started_{false};
};

}  // namespace

std::unique_ptr<IpcTransport> makeNamedPipeTransport(
    std::filesystem::path projectRoot) {
  return std::make_unique<NamedPipeTransport>(std::move(projectRoot));
}

std::unique_ptr<IpcTransport> makePlatformTransport(
    std::filesystem::path projectRoot) {
#ifdef _WIN32
  return makeNamedPipeTransport(std::move(projectRoot));
#else
  return makeUdsTransport(std::move(projectRoot));
#endif
}

}  // namespace ultra::ipc
