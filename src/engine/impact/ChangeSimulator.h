#pragma once

#include "ImpactTypes.h"

#include <vector>

namespace ultra::engine::impact {

class ChangeSimulator {
 public:
  [[nodiscard]] SimulationResult simulateSymbolChange(
      const ImpactPrediction& prediction) const;
  [[nodiscard]] SimulationResult simulateFileChange(
      const ImpactPrediction& prediction) const;

 private:
  static std::vector<std::string> collectPotentialBreakages(
      const ImpactPrediction& prediction);
};

}  // namespace ultra::engine::impact
