#include "ultra/runtime/snapshot_store.h"

#include <utility>

namespace ultra::runtime {

SnapshotStore::SnapshotStore() : slot_(nullptr) {}

GraphSnapshot SnapshotStore::current() const {
  const std::shared_ptr<const SnapshotSlot> slot =
      slot_.load(std::memory_order_acquire);
  if (!slot) {
    return {};
  }
  return slot->snapshot;
}

void SnapshotStore::publish(const GraphSnapshot& snapshot) {
  auto slot = std::make_shared<SnapshotSlot>();
  slot->snapshot = snapshot;
  slot_.store(std::move(slot), std::memory_order_release);
}

bool SnapshotStore::empty() const {
  return slot_.load(std::memory_order_acquire) == nullptr;
}

}  // namespace ultra::runtime

