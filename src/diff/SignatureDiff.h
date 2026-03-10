#pragma once
//SignatureDiff.h
#include "ContractBreak.h"
#include "SymbolDelta.h"
#include "../graph/DependencyGraph.h"

#include <cstdint>
#include <vector>

namespace ultra::memory {
class SemanticMemory;
}

namespace ultra::diff {

/// Engine for detecting API contract breaks from structural deltas.
class SignatureDiff {
 public:
  /// Analyzes a set of symbol deltas for breaking contract changes.
  /// Uses the dependency graph to identify affected callers.
  static std::vector<ContractBreak> analyze(
      const std::vector<SymbolDelta>& deltas,
      const ultra::graph::DependencyGraph& depGraph,
      ultra::memory::SemanticMemory* semanticMemory = nullptr,
      std::uint64_t semanticVersion = 0U);
};

}  // namespace ultra::diff
