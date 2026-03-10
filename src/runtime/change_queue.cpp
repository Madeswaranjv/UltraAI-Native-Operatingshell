#include "change_queue.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ultra::runtime {

namespace {

DaemonEventType toDaemonEventType(const ChangeType type) {
  switch (type) {
    case ChangeType::Added:
      return DaemonEventType::FileAdded;
    case ChangeType::Modified:
      return DaemonEventType::FileModified;
    case ChangeType::Removed:
      return DaemonEventType::FileRemoved;
  }
  return DaemonEventType::FileModified;
}

bool toChangeEvent(const DaemonEvent& event, ChangeEvent& outEvent) {
  switch (event.type) {
    case DaemonEventType::FileAdded:
      outEvent.type = ChangeType::Added;
      outEvent.path = event.path;
      return true;
    case DaemonEventType::FileModified:
      outEvent.type = ChangeType::Modified;
      outEvent.path = event.path;
      return true;
    case DaemonEventType::FileRemoved:
      outEvent.type = ChangeType::Removed;
      outEvent.path = event.path;
      return true;
    case DaemonEventType::RebuildRequest:
    case DaemonEventType::ShutdownRequest:
      return false;
  }
  return false;
}

}  // namespace

void ChangeQueue::push(ChangeEvent event) {
  controller_.enqueue(toDaemonEventType(event.type), std::move(event.path));
}

bool ChangeQueue::popFor(ChangeEvent& eventOut,
                         const std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  std::stop_source stopSource;

  while (true) {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    if (elapsed >= timeout) {
      return false;
    }
    const auto remaining = std::max<std::chrono::milliseconds>(
        std::chrono::milliseconds(1), timeout - elapsed);

    DaemonEvent event;
    if (!controller_.dequeue(event, stopSource.get_token(), remaining)) {
      return false;
    }
    if (toChangeEvent(event, eventOut)) {
      return true;
    }
  }
}

bool ChangeQueue::popRuntimeEvent(DaemonEvent& eventOut,
                                  const std::stop_token stopToken,
                                  const std::chrono::milliseconds timeout) {
  return controller_.dequeue(eventOut, stopToken, timeout);
}

void ChangeQueue::requestRebuild() {
  controller_.enqueue(DaemonEventType::RebuildRequest);
}

void ChangeQueue::requestShutdown() {
  controller_.enqueue(DaemonEventType::ShutdownRequest);
}

void ChangeQueue::notifyAll() {
  controller_.notifyAll();
}

void ChangeQueue::clear() {
  controller_.clear();
}

std::size_t ChangeQueue::size() const {
  return controller_.pending();
}

}  // namespace ultra::runtime
