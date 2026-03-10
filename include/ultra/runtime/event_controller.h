#pragma once

#include "event_queue.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>

namespace ultra::runtime {

class EventController {
 public:
  void enqueue(DaemonEventType type, std::string path = {});
  bool dequeue(DaemonEvent& eventOut,
               std::stop_token stopToken,
               std::chrono::milliseconds timeout =
                   std::chrono::milliseconds(100));
  void clear();
  void notifyAll();
  [[nodiscard]] std::size_t pending() const;

 private:
  std::atomic<std::uint64_t> nextSequence_{1U};
  EventQueue queue_;
};

}  // namespace ultra::runtime

