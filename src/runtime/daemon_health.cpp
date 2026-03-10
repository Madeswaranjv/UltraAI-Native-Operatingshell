#include "ultra/runtime/daemon_health.h"

#include <external/json.hpp>

#include <chrono>
#include <exception>
#include <fstream>
#include <utility>

namespace ultra::runtime {

namespace {

std::uint64_t unixTimeMillisNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

}  // namespace

DaemonHealth::DaemonHealth(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      runtimeDir_(projectRoot_ / ".ultra" / "runtime"),
      ipcDir_(runtimeDir_ / "ipc"),
      lockPath_(runtimeDir_ / "daemon.lock"),
      heartbeatPath_(runtimeDir_ / "daemon_heartbeat"),
      socketPath_(runtimeDir_ / "daemon.sock"),
      compatLockPath_(ipcDir_ / "daemon.lock"),
      compatHeartbeatPath_(ipcDir_ / "heartbeat.json") {}

bool DaemonHealth::initialize(const unsigned long pid, std::string& error) const {
  try {
    std::filesystem::create_directories(runtimeDir_);
    std::filesystem::create_directories(ipcDir_);
  } catch (const std::exception& ex) {
    error = std::string("Failed to create daemon runtime directories: ") +
            ex.what();
    return false;
  }

  nlohmann::json lockPayload;
  lockPayload["pid"] = pid;
  lockPayload["started_at"] = unixTimeMillisNow();
  lockPayload["start_timestamp"] = lockPayload["started_at"];
  lockPayload["transport_endpoint"] = socketPath_.generic_string();
  lockPayload["status"] = "running";
  const std::string serialized = lockPayload.dump(2);
  if (!writeJsonAtomically(lockPath_, serialized, error)) {
    return false;
  }
  if (!writeJsonAtomically(compatLockPath_, serialized, error)) {
    return false;
  }

  std::ofstream socketOut(socketPath_, std::ios::binary | std::ios::trunc);
  if (!socketOut) {
    error = "Failed to create daemon socket marker: " + socketPath_.string();
    return false;
  }
  socketOut << "ultra-daemon-socket";
  return true;
}

bool DaemonHealth::writeHeartbeat(const unsigned long pid,
                                  const DaemonHeartbeatSnapshot& snapshot,
                                  std::string& error) const {
  nlohmann::json payload;
  payload["pid"] = pid;
  payload["lastHeartbeat"] = snapshot.lastHeartbeatMs == 0U
                                 ? unixTimeMillisNow()
                                 : snapshot.lastHeartbeatMs;
  payload["runtimeActive"] = snapshot.runtimeActive;
  payload["indexedFiles"] = snapshot.indexedFiles;
  payload["symbolsIndexed"] = snapshot.symbolsIndexed;
  payload["dependenciesIndexed"] = snapshot.dependenciesIndexed;
  payload["graphNodes"] = snapshot.graphNodes;
  payload["graphEdges"] = snapshot.graphEdges;
  payload["pendingChanges"] = snapshot.pendingChanges;
  payload["hotSliceSize"] = snapshot.hotSliceSize;
  payload["snapshotCount"] = snapshot.snapshotCount;
  payload["activeBranchCount"] = snapshot.activeBranchCount;
  payload["memoryUsageBytes"] = snapshot.memoryUsageBytes;
  const std::string serialized = payload.dump(2);

  if (!writeJsonAtomically(heartbeatPath_, serialized, error)) {
    return false;
  }
  return writeJsonAtomically(compatHeartbeatPath_, serialized, error);
}

void DaemonHealth::shutdown() const {
  std::error_code ec;
  std::filesystem::remove(lockPath_, ec);
  std::filesystem::remove(heartbeatPath_, ec);
  std::filesystem::remove(socketPath_, ec);
  std::filesystem::remove(compatLockPath_, ec);
  std::filesystem::remove(compatHeartbeatPath_, ec);
}

const std::filesystem::path& DaemonHealth::lockPath() const noexcept {
  return lockPath_;
}

const std::filesystem::path& DaemonHealth::heartbeatPath() const noexcept {
  return heartbeatPath_;
}

const std::filesystem::path& DaemonHealth::socketPath() const noexcept {
  return socketPath_;
}

std::chrono::milliseconds DaemonHealth::heartbeatInterval() noexcept {
  return std::chrono::seconds(2);
}

bool DaemonHealth::writeJsonAtomically(const std::filesystem::path& outputPath,
                                       const std::string& payload,
                                       std::string& error) const {
  try {
    const std::filesystem::path tmpPath = outputPath.string() + ".tmp";
    {
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "Failed to open temporary daemon health file: " +
                tmpPath.string();
        return false;
      }
      out << payload;
      out.flush();
      if (!out) {
        error = "Failed to write daemon health file: " + tmpPath.string();
        return false;
      }
    }

    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
    std::filesystem::rename(tmpPath, outputPath);
    return true;
  } catch (const std::exception& ex) {
    error = std::string("Failed to persist daemon health payload: ") +
            ex.what();
    return false;
  }
}

}  // namespace ultra::runtime
