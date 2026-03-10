#pragma once
//E:\Projects\Ultra\src\memory\epoch\EpochGuard.h
#include "EpochManager.h"

namespace ultra::memory::epoch {

class EpochGuard {
 public:
  explicit EpochGuard(EpochManager& manager = EpochManager::instance())
      : manager_(&manager), active_(true) {
    manager_->enterRead();
  }

  EpochGuard(const EpochGuard&) = delete;
  EpochGuard& operator=(const EpochGuard&) = delete;

  EpochGuard(EpochGuard&& other) noexcept
      : manager_(other.manager_), active_(other.active_) {
    other.manager_ = nullptr;
    other.active_ = false;
  }

  EpochGuard& operator=(EpochGuard&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    release();
    manager_ = other.manager_;
    active_ = other.active_;
    other.manager_ = nullptr;
    other.active_ = false;
    return *this;
  }

  ~EpochGuard() { release(); }

 private:
  void release() noexcept {
    if (!active_ || manager_ == nullptr) {
      return;
    }
    manager_->exitRead();
    active_ = false;
  }

  EpochManager* manager_;
  bool active_;
};

}  // namespace ultra::memory::epoch

