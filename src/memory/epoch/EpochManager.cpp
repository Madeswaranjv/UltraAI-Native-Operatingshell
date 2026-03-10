#include "EpochManager.h"

#include <algorithm>
#include <iterator>
//E:\Projects\Ultra\src\memory\epoch\EpochManager.cpp
namespace ultra::memory::epoch {

thread_local std::uint64_t EpochManager::localEpoch_ = EpochManager::kInactiveEpoch;
thread_local std::uint32_t EpochManager::localReadDepth_ = 0U;
thread_local EpochManager::ThreadRecord* EpochManager::localRecord_ = nullptr;
thread_local std::vector<EpochManager::RetiredObject>
    EpochManager::localRetiredObjects_{};

EpochManager& EpochManager::instance() {
  static EpochManager manager;
  return manager;
}

void EpochManager::enterRead() {
  ThreadRecord& record = ensureThreadRecord();
  if (localReadDepth_ > 0U) {
    ++localReadDepth_;
    return;
  }

  const std::uint64_t epoch = globalEpoch_.load(std::memory_order_acquire);
  localEpoch_ = epoch;
  record.activeEpoch.store(epoch, std::memory_order_release);
  localReadDepth_ = 1U;
}

void EpochManager::exitRead() {
  if (localRecord_ == nullptr || localReadDepth_ == 0U) {
    return;
  }

  --localReadDepth_;
  if (localReadDepth_ > 0U) {
    return;
  }

  localEpoch_ = kInactiveEpoch;
  localRecord_->activeEpoch.store(kInactiveEpoch, std::memory_order_release);
}

void EpochManager::advanceEpoch() {
  globalEpoch_.fetch_add(1U, std::memory_order_acq_rel);
}

void EpochManager::retire(void* ptr, const Deleter deleter) {
  if (ptr == nullptr || deleter == nullptr) {
    return;
  }

  (void)ensureThreadRecord();
  RetiredObject retired;
  retired.pointer = ptr;
  retired.retireEpoch = globalEpoch_.load(std::memory_order_acquire);
  retired.deleter = deleter;
  localRetiredObjects_.push_back(retired);

  if (localRetiredObjects_.size() >= kLocalRetireFlushThreshold) {
    flushThreadLocalRetired();
  }

  if (retiredApprox_.load(std::memory_order_relaxed) >= kGlobalReclaimThreshold) {
    advanceEpoch();
    reclaim();
  }
}

void EpochManager::reclaim() {
  flushThreadLocalRetired();

  const std::uint64_t oldestReaderEpoch = oldestActiveReaderEpoch();

  std::vector<RetiredObject> reclaimable;
  {
    std::lock_guard lock(retiredMutex_);
    if (retiredObjects_.empty()) {
      retiredApprox_.store(0U, std::memory_order_release);
      return;
    }

    std::vector<RetiredObject> survivors;
    survivors.reserve(retiredObjects_.size());
    reclaimable.reserve(retiredObjects_.size());
    for (const RetiredObject& retired : retiredObjects_) {
      if (retired.retireEpoch < oldestReaderEpoch) {
        reclaimable.push_back(retired);
      } else {
        survivors.push_back(retired);
      }
    }

    retiredObjects_.swap(survivors);
    retiredApprox_.store(retiredObjects_.size(), std::memory_order_release);
  }

  for (const RetiredObject& retired : reclaimable) {
    retired.deleter(retired.pointer);
  }
}

std::uint64_t EpochManager::currentEpoch() const noexcept {
  return globalEpoch_.load(std::memory_order_acquire);
}

std::size_t EpochManager::retiredCount() const noexcept {
  return retiredApprox_.load(std::memory_order_acquire) +
         localRetiredObjects_.size();
}

EpochManager::ThreadRecord& EpochManager::ensureThreadRecord() {
  if (localRecord_ != nullptr) {
    return *localRecord_;
  }

  auto* record = new ThreadRecord();
  {
    std::lock_guard lock(registryMutex_);
    threadRecords_.push_back(record);
  }
  localRecord_ = record;
  return *record;
}

std::uint64_t EpochManager::oldestActiveReaderEpoch() const {
  const std::uint64_t currentEpoch = globalEpoch_.load(std::memory_order_acquire);
  std::uint64_t oldest = currentEpoch + 1U;

  std::lock_guard lock(registryMutex_);
  for (ThreadRecord* record : threadRecords_) {
    if (record == nullptr) {
      continue;
    }
    const std::uint64_t active = record->activeEpoch.load(std::memory_order_acquire);
    if (active == kInactiveEpoch) {
      continue;
    }
    if (active < oldest) {
      oldest = active;
    }
  }

  return oldest;
}

void EpochManager::flushThreadLocalRetired() {
  if (localRetiredObjects_.empty()) {
    return;
  }

  std::lock_guard lock(retiredMutex_);
  retiredObjects_.insert(retiredObjects_.end(),
                         std::make_move_iterator(localRetiredObjects_.begin()),
                         std::make_move_iterator(localRetiredObjects_.end()));
  retiredApprox_.store(retiredObjects_.size(), std::memory_order_release);
  localRetiredObjects_.clear();
}

}  // namespace ultra::memory::epoch
