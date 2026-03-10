#pragma once
//worker_pool.h
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <thread>
#include <mutex>
#include <vector>
#include<stop_token>
namespace ultra::runtime {

class WorkerPool {
 public:
  explicit WorkerPool(std::size_t requestedThreads = 0U);
  ~WorkerPool();

  void start();
  void stop();

  bool submit(std::function<void()> task);

  [[nodiscard]] std::size_t threadCount() const noexcept;
  [[nodiscard]] std::size_t pendingTasks() const;

  [[nodiscard]] static std::size_t recommendedThreadCount() noexcept;

 private:
  void workerLoop(std::stop_token stopToken);

  std::size_t requestedThreads_{0U};
  std::atomic<bool> running_{false};

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::jthread> workers_;
};

}  // namespace ultra::runtime

