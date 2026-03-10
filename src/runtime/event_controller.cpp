#include "ultra/runtime/event_controller.h"

#include <utility>

namespace ultra::runtime {

void EventController::enqueue(const DaemonEventType type, std::string path) {
  DaemonEvent event;
  event.type = type;
  event.path = std::move(path);
  event.sequence = nextSequence_.fetch_add(1U, std::memory_order_relaxed);
  event.queuedAt = std::chrono::steady_clock::now();
  queue_.push(std::move(event));
}

bool EventController::dequeue(DaemonEvent& eventOut,
                              const std::stop_token stopToken,
                              const std::chrono::milliseconds timeout) {
  return queue_.pop(eventOut, stopToken, timeout);
}

void EventController::clear() {
  queue_.clear();
}

void EventController::notifyAll() {
  queue_.notifyAll();
}

std::size_t EventController::pending() const {
  return queue_.size();
}

}  // namespace ultra::runtime

