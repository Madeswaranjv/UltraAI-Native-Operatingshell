#include "WeightTuner.h"

#include <algorithm>
#include <cmath>

namespace ultra::calibration {

WeightTuner::WeightTuner(WeightManager& manager) : manager_(manager) {}

void WeightTuner::tune(const std::string& weightName, float feedbackSignal) {
  // Clamp feedback to [-1.0, 1.0]
  feedbackSignal = std::clamp(feedbackSignal, -1.0f, 1.0f);

  float currentWeight = manager_.getWeight(weightName, 1.0f);
  
  // Calculate delta: positive feedback increases weight, negative decreases.
  float delta = currentWeight * learningRate_ * feedbackSignal;
  float newWeight = std::clamp(currentWeight + delta, minWeight_, maxWeight_);
  
  manager_.setWeight(weightName, newWeight);
}

float WeightTuner::tuneSmoothed(const std::string& weightName,
                                float feedbackSignal,
                                float smoothing) {
  feedbackSignal = std::clamp(feedbackSignal, -1.0f, 1.0f);
  if (!std::isfinite(smoothing)) {
    smoothing = 0.10f;
  }
  smoothing = std::clamp(smoothing, 0.0f, 1.0f);

  const float currentWeight = manager_.getWeight(weightName, 1.0f);
  const float delta = currentWeight * learningRate_ * feedbackSignal;
  const float targetWeight =
      std::clamp(currentWeight + delta, minWeight_, maxWeight_);
  const float blendedWeight = std::clamp(
      (currentWeight * (1.0f - smoothing)) + (targetWeight * smoothing),
      minWeight_, maxWeight_);

  manager_.setWeight(weightName, blendedWeight);
  return blendedWeight;
}

std::map<std::string, float> WeightTuner::tuneBatchSmoothed(
    const std::map<std::string, float>& feedbackSignals,
    float smoothing) {
  std::map<std::string, float> adjustedWeights;
  for (const auto& [weightName, feedbackSignal] : feedbackSignals) {
    adjustedWeights.emplace(weightName,
                            tuneSmoothed(weightName, feedbackSignal, smoothing));
  }
  return adjustedWeights;
}

}  // namespace ultra::calibration
