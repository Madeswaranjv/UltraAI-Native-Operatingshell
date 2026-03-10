#pragma once

#include "runtime/GraphSnapshot.h"

#include <atomic>
#include <memory>

namespace ultra::runtime {

class SnapshotStore {
 public:
  SnapshotStore();

  [[nodiscard]] GraphSnapshot current() const;
  void publish(const GraphSnapshot& snapshot);
  [[nodiscard]] bool empty() const;

 private:
  struct SnapshotSlot {
    GraphSnapshot snapshot;
  };

  std::atomic<std::shared_ptr<const SnapshotSlot>> slot_;
};

}  // namespace ultra::runtime

