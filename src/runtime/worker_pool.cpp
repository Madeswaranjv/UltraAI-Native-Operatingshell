#include "ultra/runtime/worker_pool.h"
#include "CPUGovernor.h"

#include <algorithm>
#include <thread>
#include <utility>
#include<stop_token>
//E:\Projects\Ultra\src\runtime\worker_pool.cpp
namespace ultra::runtime {

WorkerPool::WorkerPool(const std::size_t requestedThreads)
    : requestedThreads_(requestedThreads) {}

WorkerPool::~WorkerPool() {
  stop();
}

void WorkerPool::start() {
  if (running_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  const std::size_t targetThreads =
      requestedThreads_ == 0U ? recommendedThreadCount() : requestedThreads_;
  workers_.reserve(targetThreads);
  for (std::size_t index = 0U; index < targetThreads; ++index) {
    workers_.emplace_back([this](std::stop_token stopToken) {
      workerLoop(stopToken);
    });
  }
}

void WorkerPool::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.clear();
  }
  cv_.notify_all();

  for (std::jthread& worker : workers_) {
    worker.request_stop();
  }
  workers_.clear();
}

bool WorkerPool::submit(std::function<void()> task) {
  if (!task) {
    return false;
  }
  if (!running_.load(std::memory_order_acquire)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  cv_.notify_one();
  return true;
}

std::size_t WorkerPool::threadCount() const noexcept {
  return workers_.size();
}

std::size_t WorkerPool::pendingTasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

std::size_t WorkerPool::recommendedThreadCount() noexcept {
  const std::size_t recommended =
      CPUGovernor::instance().recommendedThreadCount();
  return std::max<std::size_t>(1U, recommended);
}

void WorkerPool::workerLoop(const std::stop_token stopToken) {
  [[maybe_unused]] std::stop_callback callback(stopToken, [this]() {
    cv_.notify_all();
  });

  while (!stopToken.stop_requested()) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this, stopToken]() {
        return stopToken.stop_requested() || !tasks_.empty() ||
               !running_.load(std::memory_order_acquire);
      });
      if (stopToken.stop_requested() ||
          !running_.load(std::memory_order_acquire)) {
        return;
      }
      if (tasks_.empty()) {
        continue;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }

    try {
      task();
    } catch (...) {
      // Worker tasks are best-effort and should not terminate the daemon.
    }
  }
}

}  // namespace ultra::runtime
