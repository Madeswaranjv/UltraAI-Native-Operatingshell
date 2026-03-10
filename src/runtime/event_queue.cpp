#include "ultra/runtime/event_queue.h"

#include <utility>

namespace ultra::runtime {

void EventQueue::push(DaemonEvent event) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(event));
  }
  cv_.notify_one();
}

bool EventQueue::pop(DaemonEvent& eventOut,
                     const std::stop_token stopToken,
                     const std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  [[maybe_unused]] std::stop_callback callback(stopToken, [this]() {
    cv_.notify_all();
  });

  const bool notified = cv_.wait_for(lock, timeout, [this, stopToken]() {
    return !queue_.empty() || stopToken.stop_requested();
  });
  if (!notified || stopToken.stop_requested() || queue_.empty()) {
    return false;
  }

  eventOut = std::move(queue_.front());
  queue_.pop_front();
  return true;
}

void EventQueue::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.clear();
}

void EventQueue::notifyAll() {
  cv_.notify_all();
}

std::size_t EventQueue::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

}  // namespace ultra::runtime
