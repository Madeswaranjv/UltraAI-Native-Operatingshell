#pragma once

#include "ultra/runtime/event_controller.h"
#include "ultra/runtime/event_types.h"

#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>

namespace ultra::runtime {

enum class ChangeType { Added, Modified, Removed };

struct ChangeEvent {
  ChangeType type{ChangeType::Modified};
  std::string path;
};

class ChangeQueue {
 public:
  void push(ChangeEvent event);
  bool popFor(ChangeEvent& eventOut, std::chrono::milliseconds timeout);
  bool popRuntimeEvent(DaemonEvent& eventOut,
                       std::stop_token stopToken,
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds(100));
  void requestRebuild();
  void requestShutdown();
  void notifyAll();
  void clear();
  [[nodiscard]] std::size_t size() const;

 private:
  EventController controller_;
};

}  // namespace ultra::runtime
