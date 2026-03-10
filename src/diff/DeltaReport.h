#pragma once

#include "SymbolDelta.h"
#include "ContractBreak.h"
#include "RiskReport.h"
#include "ImpactReport.h"
//DeltaReport.h
#include <vector>

namespace ultra::diff {

/// A comprehensive change detection and impact analysis report.
struct DeltaReport {
  /// The specific symbol-level additions, removals, and modifications.
  std::vector<SymbolDelta> changeObject;

  /// Map of downstream impact through the dependency graph.
  ImpactReport impactMap;

  /// Quantitative metrics of risk, drift, and structural instability.
  RiskReport riskScore;

  /// List of severe API signature changes that will break downstream.
  std::vector<ContractBreak> contractBreaks;
};

}  // namespace ultra::diff
