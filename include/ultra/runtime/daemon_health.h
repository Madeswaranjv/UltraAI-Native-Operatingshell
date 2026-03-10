#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ultra::runtime {

struct DaemonHeartbeatSnapshot {
  bool runtimeActive{false};
  std::uint64_t lastHeartbeatMs{0U};
  std::size_t indexedFiles{0U};
  std::size_t symbolsIndexed{0U};
  std::size_t dependenciesIndexed{0U};
  std::size_t graphNodes{0U};
  std::size_t graphEdges{0U};
  std::size_t pendingChanges{0U};
  std::size_t hotSliceSize{0U};
  std::size_t snapshotCount{0U};
  std::size_t activeBranchCount{0U};
  std::size_t memoryUsageBytes{0U};
};

class DaemonHealth {
 public:
  explicit DaemonHealth(std::filesystem::path projectRoot);

  bool initialize(unsigned long pid, std::string& error) const;
  bool writeHeartbeat(unsigned long pid,
                      const DaemonHeartbeatSnapshot& snapshot,
                      std::string& error) const;
  void shutdown() const;

  [[nodiscard]] const std::filesystem::path& lockPath() const noexcept;
  [[nodiscard]] const std::filesystem::path& heartbeatPath() const noexcept;
  [[nodiscard]] const std::filesystem::path& socketPath() const noexcept;

  [[nodiscard]] static std::chrono::milliseconds heartbeatInterval() noexcept;

 private:
  bool writeJsonAtomically(const std::filesystem::path& outputPath,
                           const std::string& payload,
                           std::string& error) const;

  std::filesystem::path projectRoot_;
  std::filesystem::path runtimeDir_;
  std::filesystem::path ipcDir_;
  std::filesystem::path lockPath_;
  std::filesystem::path heartbeatPath_;
  std::filesystem::path socketPath_;
  std::filesystem::path compatLockPath_;
  std::filesystem::path compatHeartbeatPath_;
};

}  // namespace ultra::runtime

