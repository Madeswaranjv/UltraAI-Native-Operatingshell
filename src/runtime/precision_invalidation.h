#pragma once

#include "GraphSnapshot.h"
#include "StructuralChange.h"

#include "../ai/RuntimeState.h"
#include "../diff/DeltaReport.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ultra::memory {
class SemanticMemory;
}

namespace ultra::runtime {

using SymbolID = std::uint64_t;

struct DiffResult {
  diff::DeltaReport delta;
  std::vector<std::pair<std::string, std::string>> addedDependencyEdges;
  std::vector<std::pair<std::string, std::string>> removedDependencyEdges;
  std::vector<std::string> changedFiles;
  std::vector<SymbolID> affectedSymbols;
  std::vector<std::pair<SymbolID, std::string>> renamedSymbols;
};

DiffResult buildDiffResult(const ai::RuntimeState& previousState,
                           const ai::RuntimeState& currentState,
                           ultra::memory::SemanticMemory* semanticMemory = nullptr,
                           std::uint64_t semanticVersion = 0U);

StructuralChangeType classifyChange(const DiffResult& diff);

std::vector<SymbolID> computeImpactDepthLimited(const GraphSnapshot& snapshot,
                                                SymbolID root,
                                                std::size_t depthLimit);

}  // namespace ultra::runtime
