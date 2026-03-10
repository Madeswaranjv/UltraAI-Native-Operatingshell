#pragma once
//RiskScorer.h
#include "RiskReport.h"
#include "SymbolDelta.h"
#include "../graph/DependencyGraph.h"

#include <vector>

namespace ultra::diff {

/// Scorer engine that determines risk metrics of code changes.
class RiskScorer {
 public:
  /// Computes a comprehensive risk report for a set of symbol deltas.
  /// Factors in dependency fan-out, modification types, and structure.
  static RiskReport score(const std::vector<SymbolDelta>& deltas,
                          const ultra::graph::DependencyGraph& depGraph);
};

}  // namespace ultra::diff
