#pragma once

// INTERNAL — DO NOT EXPOSE OUTSIDE KERNEL

#include "../../memory/StateGraph.h"
#include "../../memory/epoch/EpochGuard.h"
#include "../../types/BranchId.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace ultra::ai {
struct RuntimeState;
}

namespace ultra::core::graph_store {
class GraphStore;
}

namespace ultra::runtime {

using BranchId = ultra::types::BranchId;

struct GraphSnapshot {
  std::shared_ptr<const memory::StateGraph> graph;
  std::shared_ptr<const ai::RuntimeState> runtimeState;
  core::graph_store::GraphStore* graphStore{nullptr};
  std::uint64_t version{0};
  BranchId branch{BranchId::nil()};

  [[nodiscard]] std::string deterministicHash() const {
    if (!graph) {
      throw std::logic_error("GraphSnapshot is missing graph data.");
    }
    memory::epoch::EpochGuard guard(memory::epoch::EpochManager::instance());
    return graph->getDeterministicHash();
  }
};

}  // namespace ultra::runtime
