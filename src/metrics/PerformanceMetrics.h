#pragma once

#include <external/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ultra::metrics {

struct SnapshotMetrics {
  std::string operation;
  std::uint64_t durationMicros{0U};
  std::size_t nodeCount{0U};
  std::size_t edgeCount{0U};
  std::size_t snapshotSizeBytes{0U};
};

struct ContextMetrics {
  std::uint64_t durationMicros{0U};
  std::size_t candidateSymbolCount{0U};
  std::size_t selectedSymbolCount{0U};
  std::size_t jsonSizeBytes{0U};
  std::size_t estimatedTokens{0U};
  std::size_t rawEstimatedTokens{0U};
  double truncationRatio{0.0};
  std::size_t hotSliceHits{0U};
  std::size_t hotSliceLookups{0U};
};

struct BranchMetrics {
  std::string operation;
  std::uint64_t durationMicros{0U};
  std::size_t overlayResidentCount{0U};
  std::size_t evictionCount{0U};
  std::size_t branchCountBefore{0U};
  std::size_t branchCountAfter{0U};
};

struct TokenSavingsMetrics {
  std::uint64_t rawTokensTotal{0U};
  std::uint64_t compressedTokensTotal{0U};
  std::uint64_t savedTokensTotal{0U};
  std::size_t sampleCount{0U};
  double cumulativeSavingsRatio{0.0};
};

struct MemoryGovernanceMetrics {
  std::uint64_t snapshotVersion{0U};
  std::string branchId;
  std::size_t activeOverlayCount{0U};
  std::size_t activeOverlayLimit{0U};
  std::size_t hotSliceCurrentSize{0U};
  std::size_t hotSliceTargetCapacity{0U};
  std::size_t hotSliceEvictionCount{0U};
  std::size_t hotSliceRecalibrationCount{0U};
  double hotSliceHitRate{0.0};
  double contextReuseRate{0.0};
  double tokenBudgetScale{1.0};
  std::size_t compressionDepth{1U};
  double pruningThreshold{0.0};
  double impactPredictionAccuracy{0.0};
};

class PerformanceMetrics {
 public:
  static void configureFromEnvironment();
  static void setEnabled(bool enabled);
  [[nodiscard]] static bool isEnabled();

  static void recordSnapshotMetric(const SnapshotMetrics& metric);
  static void recordContextMetric(const ContextMetrics& metric);
  static void recordBranchMetric(const BranchMetrics& metric);

  static void recordTokenSavings(std::size_t rawTokens,
                                 std::size_t compressedTokens);
  static void recordOverlayReuse(bool reused);
  static void recordSnapshotReuse();
  static void recordHotSliceLookup(std::size_t hits, std::size_t lookups);
  static void recordContextReuse(std::size_t reused, std::size_t total);
  static void recordImpactPredictionAccuracy(double accuracy);
  static void recordMemoryGovernance(const MemoryGovernanceMetrics& metric);
  static void recordWeightAdjustment(const std::string& weightName,
                                     double previousValue,
                                     double newValue);

  [[nodiscard]] static double averageTokenSavingsRatio();
  [[nodiscard]] static nlohmann::ordered_json report();
  static void reset();

 private:
  static constexpr std::size_t kMaxSampleCount = 4096U;
  static constexpr std::size_t kEstimatedTokensPerCall = 4000U;

  template <typename T>
  static void pushBounded(std::vector<T>& storage, const T& value) {
    if (storage.size() >= kMaxSampleCount) {
      storage.erase(storage.begin());
    }
    storage.push_back(value);
  }

  static std::atomic<bool> s_enabled_;
  static std::once_flag s_envConfigured_;
  static std::mutex s_mutex_;
  static std::vector<SnapshotMetrics> s_snapshotMetrics_;
  static std::vector<ContextMetrics> s_contextMetrics_;
  static std::vector<BranchMetrics> s_branchMetrics_;
  static TokenSavingsMetrics s_tokenSavings_;
  static MemoryGovernanceMetrics s_memoryGovernance_;
  static std::size_t s_overlayReuseCount_;
  static std::size_t s_overlayReloadCount_;
  static std::size_t s_snapshotReuseCount_;
  static std::size_t s_hotSliceHits_;
  static std::size_t s_hotSliceLookups_;
  static std::size_t s_contextReuseHits_;
  static std::size_t s_contextReuseLookups_;
  static std::vector<double> s_impactPredictionAccuracySamples_;
  static std::map<std::string, std::pair<double, double>> s_weightAdjustments_;
  static std::size_t s_weightAdjustmentCount_;
};

}  // namespace ultra::metrics
