#pragma once
//ImpactAnalyzer.h
#include "ImpactReport.h"
#include "SymbolDelta.h"
#include "../ai/SymbolTable.h"
#include "../graph/DependencyGraph.h"

#include <vector>

namespace ultra::diff {

/// Engine for evaluating downstream impact of code changes.
class ImpactAnalyzer {
 public:
  /// Analyzes changes and traces them through the dependency graph
  /// to predict regressions and structural risk.
  static ImpactReport analyze(const std::vector<SymbolDelta>& deltas,
                              const ultra::graph::DependencyGraph& depGraph,
                              const std::vector<ultra::ai::SymbolRecord>& oldSymbols);
};

}  // namespace ultra::diff
