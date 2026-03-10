#pragma once

#include "WeightManager.h"
#include <string>

namespace ultra::calibration {

/// Applies heuristic bias adjustments to weights based on detected patterns.
class BiasAdjuster {
 public:
  explicit BiasAdjuster(WeightManager& manager);

  /// Apply a short-term bias boost to specific metrics if a pattern suggests 
  /// the user is engaging in a dense, specialized task (e.g., repeatedly diffing).
  void applyPatternBias(const std::string& detectedPattern);

 private:
  WeightManager& manager_;
};

}  // namespace ultra::calibration
