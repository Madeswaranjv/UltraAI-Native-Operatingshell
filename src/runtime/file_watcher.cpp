#include "file_watcher.h"
//file_watcher.cpp
#include "../ai/FileRegistry.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef __linux__
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace ultra::runtime {

FileWatcher::FileWatcher(std::filesystem::path projectRoot,
                         ChangeQueue& changeQueue)
    : projectRoot_(
          std::filesystem::absolute(std::move(projectRoot)).lexically_normal()),
      changeQueue_(changeQueue) {}

FileWatcher::~FileWatcher() {
  stop();
}

bool FileWatcher::start(std::string& error) {
  (void)error;
  if (running_.load(std::memory_order_acquire)) {
    return true;
  }

  running_.store(true, std::memory_order_release);
  watcherThread_ = std::jthread([this](std::stop_token stopToken) {
    try {
      watchLoop(stopToken);
    } catch (...) {
      running_.store(false, std::memory_order_release);
    }
  });
  return true;
}

void FileWatcher::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (watcherThread_.joinable()) {
    watcherThread_.request_stop();
    watcherThread_.join();
  }
}

bool FileWatcher::running() const noexcept {
  return running_.load(std::memory_order_acquire);
}

void FileWatcher::watchLoop(const std::stop_token stopToken) {
// Route by platform watcher wrapper. The current backend is polling across
// platforms so daemon behavior is deterministic in this codebase.
#if defined(_WIN32)
  runWindowsWatcher(stopToken);
#elif defined(__linux__)
  runLinuxWatcher(stopToken);
#elif defined(__APPLE__)
  runMacWatcher(stopToken);
#else
  runPollingWatcher(stopToken);
#endif
}

void FileWatcher::runWindowsWatcher(const std::stop_token stopToken) {
#ifdef _WIN32
  const HANDLE directoryHandle = ::CreateFileW(
      projectRoot_.wstring().c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
      nullptr);
  if (directoryHandle == INVALID_HANDLE_VALUE) {
    runPollingWatcher(stopToken);
    return;
  }

  const HANDLE eventHandle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (eventHandle == nullptr) {
    ::CloseHandle(directoryHandle);
    runPollingWatcher(stopToken);
    return;
  }

  std::array<std::byte, 64U * 1024U> buffer{};
  OVERLAPPED overlapped{};
  overlapped.hEvent = eventHandle;

  while (running_.load(std::memory_order_acquire) &&
         !stopToken.stop_requested()) {
    ::ResetEvent(eventHandle);
    const BOOL started = ::ReadDirectoryChangesW(
        directoryHandle, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_DIR_NAME,
        nullptr, &overlapped, nullptr);
    if (started == FALSE) {
      break;
    }

    const DWORD waitResult = ::WaitForSingleObject(eventHandle, 250U);
    if (waitResult == WAIT_TIMEOUT) {
      (void)::CancelIoEx(directoryHandle, &overlapped);
      continue;
    }
    if (waitResult != WAIT_OBJECT_0) {
      break;
    }

    DWORD bytesTransferred = 0U;
    if (::GetOverlappedResult(directoryHandle, &overlapped, &bytesTransferred,
                              FALSE) == FALSE ||
        bytesTransferred == 0U) {
      continue;
    }

    DWORD offset = 0U;
    while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytesTransferred) {
      const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(
          reinterpret_cast<const char*>(buffer.data()) + offset);
      const DWORD nameBytes = info->FileNameLength;
      const DWORD entryBytes =
          static_cast<DWORD>(offsetof(FILE_NOTIFY_INFORMATION, FileName)) +
          nameBytes;
      if (offset + entryBytes > bytesTransferred) {
        break;
      }

      const std::wstring widePath(info->FileName, nameBytes / sizeof(WCHAR));
      const std::string relativePath =
          std::filesystem::path(widePath).generic_string();
      if (!relativePath.empty() && !shouldIgnorePath(relativePath)) {
        switch (info->Action) {
          case FILE_ACTION_ADDED:
          case FILE_ACTION_RENAMED_NEW_NAME:
            changeQueue_.push(ChangeEvent{ChangeType::Added, relativePath});
            break;
          case FILE_ACTION_REMOVED:
          case FILE_ACTION_RENAMED_OLD_NAME:
            changeQueue_.push(ChangeEvent{ChangeType::Removed, relativePath});
            break;
          case FILE_ACTION_MODIFIED:
            changeQueue_.push(ChangeEvent{ChangeType::Modified, relativePath});
            break;
          default:
            break;
        }
      }

      if (info->NextEntryOffset == 0U) {
        break;
      }
      if (info->NextEntryOffset > bytesTransferred - offset) {
        break;
      }
      offset += info->NextEntryOffset;
    }
  }

  (void)::CancelIoEx(directoryHandle, nullptr);
  ::CloseHandle(eventHandle);
  ::CloseHandle(directoryHandle);

  if (running_.load(std::memory_order_acquire) && !stopToken.stop_requested()) {
    runPollingWatcher(stopToken);
  }
#else
  runPollingWatcher(stopToken);
#endif
}

void FileWatcher::runLinuxWatcher(const std::stop_token stopToken) {
#ifdef __linux__
  const int descriptor = ::inotify_init1(IN_NONBLOCK);
  if (descriptor < 0) {
    runPollingWatcher(stopToken);
    return;
  }

  const int watchDescriptor = ::inotify_add_watch(
      descriptor, projectRoot_.string().c_str(),
      IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
          IN_CLOSE_WRITE);
  if (watchDescriptor < 0) {
    (void)::close(descriptor);
    runPollingWatcher(stopToken);
    return;
  }

  std::array<char, 32U * 1024U> buffer{};
  while (running_.load(std::memory_order_acquire) &&
         !stopToken.stop_requested()) {
    struct pollfd pollFd {};
    pollFd.fd = descriptor;
    pollFd.events = POLLIN;
    const int pollResult = ::poll(&pollFd, 1, 250);
    if (pollResult <= 0) {
      continue;
    }

    const ssize_t bytesRead = ::read(descriptor, buffer.data(), buffer.size());
    if (bytesRead <= 0) {
      continue;
    }

    ssize_t offset = 0;
    while (offset < bytesRead) {
      const auto* event =
          reinterpret_cast<const struct inotify_event*>(buffer.data() + offset);
      if (event->len > 0) {
        const std::string relativePath(event->name);
        if (!shouldIgnorePath(relativePath)) {
          if ((event->mask & IN_CREATE) != 0 || (event->mask & IN_MOVED_TO) != 0) {
            changeQueue_.push(ChangeEvent{ChangeType::Added, relativePath});
          } else if ((event->mask & IN_DELETE) != 0 ||
                     (event->mask & IN_MOVED_FROM) != 0) {
            changeQueue_.push(ChangeEvent{ChangeType::Removed, relativePath});
          } else if ((event->mask & IN_MODIFY) != 0 ||
                     (event->mask & IN_CLOSE_WRITE) != 0) {
            changeQueue_.push(ChangeEvent{ChangeType::Modified, relativePath});
          }
        }
      }
      offset += sizeof(struct inotify_event) + event->len;
    }
  }

  (void)::inotify_rm_watch(descriptor, watchDescriptor);
  (void)::close(descriptor);

  if (running_.load(std::memory_order_acquire) && !stopToken.stop_requested()) {
    runPollingWatcher(stopToken);
  }
#else
  runPollingWatcher(stopToken);
#endif
}

void FileWatcher::runMacWatcher(const std::stop_token stopToken) {
  // Wrapper point for FSEvents.
  runPollingWatcher(stopToken);
}

void FileWatcher::runPollingWatcher(const std::stop_token stopToken) {
  Snapshot previous = scanSnapshot();

  while (running_.load(std::memory_order_acquire) &&
         !stopToken.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (!running_.load(std::memory_order_acquire) || stopToken.stop_requested()) {
      break;
    }

    const Snapshot current = scanSnapshot();

    for (const auto& [path, timestamp] : current) {
      const auto previousIt = previous.find(path);
      if (previousIt == previous.end()) {
        changeQueue_.push(ChangeEvent{ChangeType::Added, path});
        continue;
      }
      if (timestamp != previousIt->second) {
        changeQueue_.push(ChangeEvent{ChangeType::Modified, path});
      }
    }

    for (const auto& [path, timestamp] : previous) {
      (void)timestamp;
      if (current.find(path) == current.end()) {
        changeQueue_.push(ChangeEvent{ChangeType::Removed, path});
      }
    }

    previous = current;
  }
}

bool FileWatcher::shouldIgnorePath(const std::string& relativePath) {
  if (relativePath.empty()) {
    return true;
  }
  if (relativePath == ".ultra") {
    return true;
  }
  if (relativePath.size() > 7U && relativePath.rfind(".ultra/", 0U) == 0U) {
    return true;
  }
  return false;
}

FileWatcher::Snapshot FileWatcher::scanSnapshot() const {
  Snapshot snapshot;
  const std::vector<ai::DiscoveredFile> discovered =
      ai::FileRegistry::discoverProjectFiles(projectRoot_);
  for (const ai::DiscoveredFile& file : discovered) {
    if (shouldIgnorePath(file.relativePath)) {
      continue;
    }
    snapshot[file.relativePath] = file.lastModified;
  }
  return snapshot;
}

}  // namespace ultra::runtime
