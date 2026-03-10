#include "ipc_client.h"

#include "ipc_server.h"

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ultra::runtime {

namespace {

class RequestDepthGuard {
 public:
  explicit RequestDepthGuard(int& depth) : depth_(depth) { ++depth_; }
  ~RequestDepthGuard() { --depth_; }

 private:
  int& depth_;
};

unsigned long processId() {
#ifdef _WIN32
  return static_cast<unsigned long>(::_getpid());
#else
  return static_cast<unsigned long>(::getpid());
#endif
}

std::string makeRequestId() {
  static std::atomic<std::uint64_t> counter{0U};
  const std::uint64_t seq = counter.fetch_add(1U, std::memory_order_relaxed);
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return "req-" + std::to_string(processId()) + "-" +
         std::to_string(static_cast<unsigned long long>(now)) + "-" +
         std::to_string(static_cast<unsigned long long>(seq));
}

bool ensureHealthyDaemon(const std::filesystem::path& projectRoot,
                         const bool allowAutoStart,
                         std::string& error) {
  const DaemonHealthReport health = IpcServer::inspectDaemon(projectRoot);
  if (health.healthy()) {
    return true;
  }
  if (!allowAutoStart) {
    error = health.message.empty() ? "AI daemon is not running." : health.message;
    return false;
  }

  std::string restartError;
  if (health.state == DaemonHealthState::Stale) {
    if (!IpcServer::terminateProcessForProject(projectRoot, restartError)) {
      error = restartError;
      return false;
    }
  } else if (health.state == DaemonHealthState::Dead ||
             health.state == DaemonHealthState::Corrupt) {
    if (!IpcServer::cleanupStaleArtifacts(projectRoot, restartError)) {
      error = restartError;
      return false;
    }
  }

  if (!IpcServer::spawnDetached(projectRoot, restartError)) {
    error = restartError;
    return false;
  }

  DaemonHealthReport ready;
  if (!IpcServer::waitForHealthy(projectRoot, std::chrono::seconds(5), ready,
                                 restartError)) {
    error = restartError;
    return false;
  }
  return true;
}

}  // namespace

IpcClient::IpcClient(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      ipcDir_(projectRoot_ / ".ultra" / "runtime" / "ipc"),
      requestsDir_(ipcDir_ / "requests"),
      responsesDir_(ipcDir_ / "responses"),
      lockFile_(projectRoot_ / ".ultra" / "runtime" / "daemon.lock"),
      transport_(ultra::ipc::makePlatformTransport(projectRoot_)) {}

bool IpcClient::isDaemonRunning() const {
  return IpcServer::inspectDaemon(projectRoot_).healthy();
}

bool IpcClient::request(const std::string& command,
                        nlohmann::json& responseOut,
                        std::string& error,
                        const std::chrono::milliseconds timeout) const {
  return request(command, nlohmann::json::object(), responseOut, error,
                 timeout);
}

bool IpcClient::request(const std::string& command,
                        const nlohmann::json& requestPayload,
                        nlohmann::json& responseOut,
                        std::string& error,
                        const std::chrono::milliseconds timeout) const {
  thread_local int requestDepth = 0;
  RequestDepthGuard guard(requestDepth);
  const bool allowRetry = requestDepth == 1;

  responseOut = nlohmann::json::object();

  if (command == "sleep_ai") {
    const DaemonHealthReport precheck = IpcServer::inspectDaemon(projectRoot_);
    if (!precheck.healthy()) {
      std::string cleanupError;
      bool cleaned = false;
      if (precheck.state == DaemonHealthState::Stale) {
        cleaned = IpcServer::terminateProcessForProject(projectRoot_, cleanupError);
      } else if (precheck.state == DaemonHealthState::Dead ||
                 precheck.state == DaemonHealthState::Corrupt ||
                 precheck.state == DaemonHealthState::Missing) {
        cleaned = IpcServer::cleanupStaleArtifacts(projectRoot_, cleanupError);
      }
      if (!cleaned) {
        error = cleanupError.empty() ? precheck.message : cleanupError;
        return false;
      }
      responseOut["ok"] = true;
      responseOut["exit_code"] = 0;
      responseOut["message"] = "shutdown_complete";
      responseOut["payload"] = nlohmann::json::object();
      return true;
    }
  }

  const bool allowAutoStart = command != "sleep_ai";
  if (!ensureHealthyDaemon(projectRoot_, allowAutoStart, error)) {
    return false;
  }

  if (!transport_) {
    error = "IPC transport initialization failed.";
    return false;
  }
  if (!transport_->start(error)) {
    return false;
  }

  ultra::ipc::TransportMessage request;
  request.id = makeRequestId();
  request.command = command;
  request.payload = requestPayload.is_object() ? requestPayload
                                               : nlohmann::json::object();

  if (!transport_->send(request, timeout, responseOut, error)) {
    if (allowRetry && allowAutoStart) {
      const DaemonHealthReport health = IpcServer::inspectDaemon(projectRoot_);
      if (!health.healthy()) {
        std::string restartError;
        if (ensureHealthyDaemon(projectRoot_, true, restartError)) {
          return this->request(command, requestPayload, responseOut, error,
                               timeout);
        }
        error = restartError;
      }
    }
    return false;
  }

  const bool ok = responseOut.value("ok", false);
  if (!ok) {
    error = responseOut.value("message", "Daemon request failed.");
    return false;
  }

  if (command == "sleep_ai") {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      const DaemonHealthReport health = IpcServer::inspectDaemon(projectRoot_);
      if (health.state == DaemonHealthState::Missing ||
          health.state == DaemonHealthState::Dead) {
        responseOut["message"] = "shutdown_complete";
        responseOut["ok"] = true;
        responseOut["exit_code"] = 0;
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::string terminateError;
    if (IpcServer::terminateProcessForProject(projectRoot_, terminateError,
                                              std::chrono::seconds(5))) {
      responseOut["message"] = "shutdown_complete";
      responseOut["ok"] = true;
      responseOut["exit_code"] = 0;
      return true;
    }

    error = terminateError.empty() ? "Timed out waiting for daemon shutdown."
                                   : terminateError;
    return false;
  }

  return true;
}

}  // namespace ultra::runtime
