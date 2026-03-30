#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace ultra::runtime {

enum class DaemonEventType : std::uint8_t {
  FileAdded = 0U,
  FileModified = 1U,
  FileRemoved = 2U,
  RebuildRequest = 3U,
  ShutdownRequest = 4U,
  StructuralRebuild = 5U,
  OverlayMutation = 6U,
};

struct DaemonEvent {
  DaemonEventType type{DaemonEventType::FileModified};
  std::string path;
  std::uint64_t sequence{0U};
  std::chrono::steady_clock::time_point queuedAt{};
};

}  // namespace ultra::runtime

