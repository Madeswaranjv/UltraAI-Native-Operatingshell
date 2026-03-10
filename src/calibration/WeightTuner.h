#pragma once

#include "WeightManager.h"

#include <map>
#include <string>

namespace ultra::calibration {

/// Dynamically adjusts weights based on observed feedback signals.
class WeightTuner {
 public:
  explicit WeightTuner(WeightManager& manager);

  /// Tune a specific weight based on a feedback signal from -1.0 to 1.0.
  /// Positive signal increases the weight, negative decreases it.
  void tune(const std::string& weightName, float feedbackSignal);
  [[nodiscard]] float tuneSmoothed(const std::string& weightName,
                                   float feedbackSignal,
                                   float smoothing = 0.10f);
  [[nodiscard]] std::map<std::string, float> tuneBatchSmoothed(
      const std::map<std::string, float>& feedbackSignals,
      float smoothing = 0.10f);

 private:
  WeightManager& manager_;
  const float learningRate_{0.05f};
  const float minWeight_{0.1f};
  const float maxWeight_{5.0f};
};

}  // namespace ultra::calibration
