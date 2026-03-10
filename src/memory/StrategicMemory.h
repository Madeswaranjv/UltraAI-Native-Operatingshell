#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>
//E:\Projects\Ultra\src\memory\StrategicMemory.h
namespace ultra::calibration {
class WeightTuner;
}

namespace ultra::memory {

struct StrategicOutcome {
  std::uint64_t version{0U};
  std::string category;
  std::string subject;
  bool success{false};
  bool rolledBack{false};
  double predictedRisk{0.0};
  double observedRisk{0.0};
  double predictedConfidence{0.0};
  double observedConfidence{0.0};
  std::size_t estimatedTokens{0U};
  std::size_t compressedTokens{0U};
};

struct PolicyAdjustments {
  double riskThresholdDelta{0.0};
  double determinismFloorDelta{0.0};
  double tokenBudgetScale{1.0};
  double hotSliceCapacityScale{1.0};
  double pruningThreshold{0.10};
  std::size_t compressionDepth{1U};
  double hotSliceHitRate{0.0};
  double contextReuseRate{0.0};
  double compressionRatio{1.0};
  double impactPredictionAccuracy{0.0};
  std::map<std::string, float> weightSignals;
};

struct PerformanceSnapshot {
  double avgTokenSavingsRatio{0.0};
  double avgLatencyMs{0.0};
  double errorRate{0.0};
  double hotSliceHitRate{0.0};
  double contextReuseRate{0.0};
  double compressionRatio{0.0};
  double overlayReuseRate{0.0};
  double impactPredictionAccuracy{0.0};
};

class StrategicMemory {
 public:
  explicit StrategicMemory(std::size_t retentionLimit = 2048U);

  void recordOutcome(const StrategicOutcome& outcome);
  [[nodiscard]] PolicyAdjustments getPolicyAdjustments(
      const PerformanceSnapshot* snapshot = nullptr) const;
  [[nodiscard]] bool persistTuningState(const std::filesystem::path& path) const;
  [[nodiscard]] bool loadTuningState(const std::filesystem::path& path);
  void applyWeightTuning(
      calibration::WeightTuner& tuner,
      const PerformanceSnapshot* snapshot = nullptr) const;

  [[nodiscard]] std::vector<StrategicOutcome> snapshot() const;

 private:
  static constexpr std::uint32_t kSchemaVersion = 1U;

  mutable std::shared_mutex mutex_;
  std::vector<StrategicOutcome> outcomes_;
  std::size_t retentionLimit_{0U};

  std::uint64_t intentAttempts_{0U};
  std::uint64_t intentSuccesses_{0U};
  std::uint64_t mergeAttempts_{0U};
  std::uint64_t mergeSuccesses_{0U};
  std::uint64_t rollbackCount_{0U};
  double cumulativeCompressionEfficiency_{0.0};
  std::uint64_t compressionSamples_{0U};
  double cumulativeRiskError_{0.0};
  std::uint64_t riskSamples_{0U};
  double cumulativeConfidenceError_{0.0};
  std::uint64_t confidenceSamples_{0U};
};

}  // namespace ultra::memory
