#pragma once

#include "event_types.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stop_token>

namespace ultra::runtime {

class EventQueue {
 public:
  void push(DaemonEvent event);
  bool pop(DaemonEvent& eventOut,
           std::stop_token stopToken,
           std::chrono::milliseconds timeout =
               std::chrono::milliseconds(100));
  void clear();
  void notifyAll();
  [[nodiscard]] std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<DaemonEvent> queue_;
};

}  // namespace ultra::runtime

