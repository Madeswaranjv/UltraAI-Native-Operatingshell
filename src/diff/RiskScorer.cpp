#include "RiskScorer.h"
//RiskScorer.cpp
#include <algorithm>
#include <cmath>

namespace ultra::diff {

RiskReport RiskScorer::score(const std::vector<SymbolDelta>& deltas,
                             const ultra::graph::DependencyGraph& depGraph) {
                             
  RiskReport report;
  if (deltas.empty()) return report;

  (void)depGraph;

  double totalRiskAcc = 0.0;
  
  for (const auto& delta : deltas) {
    double risk = 0.0;

    // Base risk by change type
    if (delta.changeType == ultra::types::ChangeType::Removed) {
      risk += 0.4;
    } else if (delta.changeType == ultra::types::ChangeType::Modified) {
      risk += 0.3;
    } else if (delta.changeType == ultra::types::ChangeType::Added) {
      risk += 0.1;
    }

    // Since we don't have file paths on symbols easily accessible atm without SymbolTable reverse lookup,
    // we use a crude fan-out check for the demo version using the symbol name acting as file-ish or dependency
    // In a real implementation we lookup the symbol file and check depGraph.getDependents(file).size()
    // For now, we stub an average fanout multiplier.
    double fanOutMultiplier = 1.2; 
    
    // Check if visibility became more restrictive
    if (delta.oldRecord.visibility == ultra::ai::Visibility::Public &&
        delta.newRecord.visibility != ultra::ai::Visibility::Public &&
        delta.changeType == ultra::types::ChangeType::Modified) {
      risk += 0.5;
    }
    
    risk *= fanOutMultiplier;

    totalRiskAcc += risk;

    // If risk is notably high, flag it
    if (risk > 0.5) {
      report.highRiskChanges.push_back(delta);
    }
  }

  // Monotonic scaling with total accumulated risk (deterministic)
  constexpr double scalingFactor = 10.0; // fixed deterministic scale
  report.overallRisk = 1.0 - std::exp(-totalRiskAcc / scalingFactor);
  if (report.overallRisk < 0.0) report.overallRisk = 0.0;
  if (report.overallRisk > 1.0) report.overallRisk = 1.0;

  // Instability index scales with number of changes (bounded)
  report.instabilityIndex = static_cast<double>(deltas.size()) / 100.0; // simplistic metric
  if (report.instabilityIndex > 1.0) report.instabilityIndex = 1.0;
  report.driftScore = report.overallRisk * 0.8;

  // Compute hotspots by aggregating high-risk changes.
  // Group by rough names.
  // ... future implementation.

  return report;
}

}  // namespace ultra::diff
