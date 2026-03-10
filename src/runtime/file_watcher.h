#pragma once

#include "change_queue.h"
//file_watcher.h
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <map>
#include <stop_token>
#include <string>

namespace ultra::runtime {

class FileWatcher {
 public:
  FileWatcher(std::filesystem::path projectRoot, ChangeQueue& changeQueue);
  ~FileWatcher();

  bool start(std::string& error);
  void stop();

  [[nodiscard]] bool running() const noexcept;

 private:
  using Snapshot = std::map<std::string, std::uint64_t>;

  void watchLoop(std::stop_token stopToken);
  void runWindowsWatcher(std::stop_token stopToken);
  void runLinuxWatcher(std::stop_token stopToken);
  void runMacWatcher(std::stop_token stopToken);
  void runPollingWatcher(std::stop_token stopToken);
  static bool shouldIgnorePath(const std::string& relativePath);
  Snapshot scanSnapshot() const;

  std::filesystem::path projectRoot_;
  ChangeQueue& changeQueue_;
  std::jthread watcherThread_;
  std::atomic<bool> running_{false};
};

}  // namespace ultra::runtime
