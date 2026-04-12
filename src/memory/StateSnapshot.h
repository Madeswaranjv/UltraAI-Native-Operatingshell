#pragma once

#include "StateNode.h"
#include "StateEdge.h"
#include "../types/Timestamp.h"

#include <string>
#include <vector>
#include <cstdint>

namespace ultra::memory {

struct StateSnapshot {
  // Deterministic numeric ID
  uint64_t id{0};

  // String ID for compatibility with existing tests
  std::string snapshotId;

  // Snapshot metadata persisted alongside the graph payload
  ultra::types::Timestamp createdAt;
  std::string branchId;

  // Metadata
  std::size_t nodeCount{0};
  std::size_t edgeCount{0};
  std::string graphHash;

  // Full deterministic payload
  std::vector<StateNode> nodes;
  std::vector<StateEdge> edges;
};

}  // namespace ultra::memory
