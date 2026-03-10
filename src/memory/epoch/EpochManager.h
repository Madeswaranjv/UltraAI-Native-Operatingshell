#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <vector>
//E:\Projects\Ultra\src\memory\epoch\EpochManager.h
namespace ultra::memory::epoch {

class EpochManager {
 public:
  using Deleter = void (*)(void*);

  struct RetiredObject {
    void* pointer{nullptr};
    std::uint64_t retireEpoch{0U};
    Deleter deleter{nullptr};
  };

  static constexpr std::uint64_t kInactiveEpoch =
      std::numeric_limits<std::uint64_t>::max();
  static constexpr std::size_t kLocalRetireFlushThreshold = 32U;
  static constexpr std::size_t kGlobalReclaimThreshold = 256U;

  static EpochManager& instance();

  void enterRead();
  void exitRead();

  void advanceEpoch();
  void retire(void* ptr, Deleter deleter);
  void reclaim();

  [[nodiscard]] std::uint64_t currentEpoch() const noexcept;
  [[nodiscard]] std::size_t retiredCount() const noexcept;

 private:
  struct ThreadRecord {
    std::atomic<std::uint64_t> activeEpoch{kInactiveEpoch};
  };

  EpochManager() = default;

  EpochManager(const EpochManager&) = delete;
  EpochManager& operator=(const EpochManager&) = delete;

  ThreadRecord& ensureThreadRecord();
  std::uint64_t oldestActiveReaderEpoch() const;
  void flushThreadLocalRetired();

  std::atomic<std::uint64_t> globalEpoch_{1U};

  mutable std::mutex registryMutex_;
  std::vector<ThreadRecord*> threadRecords_;

  mutable std::mutex retiredMutex_;
  std::vector<RetiredObject> retiredObjects_;
  std::atomic<std::size_t> retiredApprox_{0U};

  static thread_local std::uint64_t localEpoch_;
  static thread_local std::uint32_t localReadDepth_;
  static thread_local ThreadRecord* localRecord_;
  static thread_local std::vector<RetiredObject> localRetiredObjects_;
};

}  // namespace ultra::memory::epoch
