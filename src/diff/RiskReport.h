#pragma once

#include "SymbolDelta.h"

#include <string>
#include <vector>

namespace ultra::diff {

/// Report containing quantitative risk assessment of an update.
struct RiskReport {
  /// Overall risk of the change (0.0 to 1.0). 
  /// High values indicate likely regressions or instable changes.
  double overallRisk{0.0};

  /// Structural drift from original architecture.
  double driftScore{0.0};

  /// Index of code instability detected in the changes.
  double instabilityIndex{0.0};

  /// List of files that are highly modified or central.
  std::vector<std::string> hotspots;

  /// List of most dangerous symbol changes detected.
  std::vector<SymbolDelta> highRiskChanges;
};

}  // namespace ultra::diff
