#include "PerformanceMetrics.h"

#include "../runtime/CPUGovernor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <numeric>
#include <string>

namespace ultra::metrics {

namespace {

bool envEnabled(const std::string& token) {
  if (token.empty()) {
    return false;
  }
  return token == "1" || token == "true" || token == "TRUE" ||
         token == "on" || token == "ON";
}

std::string readEnvVar(const char* name) {
#ifdef _WIN32
  char* buffer = nullptr;
  size_t len = 0;
  std::string value;
  if (_dupenv_s(&buffer, &len, name) == 0 && buffer != nullptr) {
    value = buffer;
    free(buffer);
  }
  return value;
#else
  const char* buffer = std::getenv(name);
  return buffer != nullptr ? std::string(buffer) : std::string{};
#endif
}

double safeRatio(std::uint64_t num, std::uint64_t den) {
  if (den == 0U) return 0.0;
  return static_cast<double>(num) / static_cast<double>(den);
}

double clampDouble(const double value,
                   const double minValue,
                   const double maxValue) {
  if (!std::isfinite(value)) {
    return minValue;
  }
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

template <typename T, typename Selector>
double averageMicros(const std::vector<T>& samples, Selector selector) {
  if (samples.empty()) {
    return 0.0;
  }
  std::uint64_t total = 0U;
  for (const T& sample : samples) {
    total += selector(sample);
  }
  return static_cast<double>(total) / static_cast<double>(samples.size());
}

template <typename T, typename Selector>
std::uint64_t maxMicros(const std::vector<T>& samples, Selector selector) {
  std::uint64_t maxValue = 0U;
  for (const T& sample : samples) {
    maxValue = std::max(maxValue, selector(sample));
  }
  return maxValue;
}

double averageTokenSavingsRatioUnlocked(const TokenSavingsMetrics& metrics) {
  if (metrics.sampleCount == 0U) {
    return 0.0;
  }
  return metrics.cumulativeSavingsRatio /
         static_cast<double>(metrics.sampleCount);
}

double averageSamples(const std::vector<double>& samples) {
  if (samples.empty()) {
    return 0.0;
  }
  const double total = std::accumulate(samples.begin(), samples.end(), 0.0);
  return total / static_cast<double>(samples.size());
}

nlohmann::ordered_json buildNodeCountDistribution(
    const std::vector<SnapshotMetrics>& metrics) {
  std::map<std::size_t, std::size_t> counts;
  for (const SnapshotMetrics& snapshot : metrics) {
    ++counts[snapshot.nodeCount];
  }

  nlohmann::ordered_json distribution = nlohmann::ordered_json::array();
  for (const auto& [nodeCount, count] : counts) {
    nlohmann::ordered_json entry;
    entry["node_count"] = nodeCount;
    entry["count"] = count;
    distribution.push_back(std::move(entry));
  }
  return distribution;
}

template <typename T>
void pushBounded(std::vector<T>& vec, const T& value) {
  constexpr std::size_t kMaxSamples = 256;
  if (vec.size() >= kMaxSamples) {
    vec.erase(vec.begin());
  }
  vec.push_back(value);
}

}  // namespace

std::atomic<bool> PerformanceMetrics::s_enabled_{false};
std::once_flag PerformanceMetrics::s_envConfigured_;
std::mutex PerformanceMetrics::s_mutex_;
std::vector<SnapshotMetrics> PerformanceMetrics::s_snapshotMetrics_;
std::vector<ContextMetrics> PerformanceMetrics::s_contextMetrics_;
std::vector<BranchMetrics> PerformanceMetrics::s_branchMetrics_;
TokenSavingsMetrics PerformanceMetrics::s_tokenSavings_;
MemoryGovernanceMetrics PerformanceMetrics::s_memoryGovernance_;
std::size_t PerformanceMetrics::s_overlayReuseCount_{0U};
std::size_t PerformanceMetrics::s_overlayReloadCount_{0U};
std::size_t PerformanceMetrics::s_snapshotReuseCount_{0U};
std::size_t PerformanceMetrics::s_hotSliceHits_{0U};
std::size_t PerformanceMetrics::s_hotSliceLookups_{0U};
std::size_t PerformanceMetrics::s_contextReuseHits_{0U};
std::size_t PerformanceMetrics::s_contextReuseLookups_{0U};
std::vector<double> PerformanceMetrics::s_impactPredictionAccuracySamples_;
std::map<std::string, std::pair<double, double>>
    PerformanceMetrics::s_weightAdjustments_;
std::size_t PerformanceMetrics::s_weightAdjustmentCount_{0U};

void PerformanceMetrics::configureFromEnvironment() {
  std::call_once(s_envConfigured_, []() {
    const std::string e1 = readEnvVar("ULTRA_METRICS_ENABLED");
    const std::string e2 = readEnvVar("ULTRA_ENABLE_METRICS");
    s_enabled_.store(envEnabled(e1) || envEnabled(e2));
  });
}

void PerformanceMetrics::setEnabled(bool enabled) {
  configureFromEnvironment();
  s_enabled_.store(enabled);
}

bool PerformanceMetrics::isEnabled() {
  configureFromEnvironment();
  return s_enabled_.load();
}

void PerformanceMetrics::recordSnapshotMetric(const SnapshotMetrics& m) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  pushBounded(s_snapshotMetrics_, m);
}

void PerformanceMetrics::recordContextMetric(const ContextMetrics& m) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  pushBounded(s_contextMetrics_, m);
}

void PerformanceMetrics::recordBranchMetric(const BranchMetrics& m) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  pushBounded(s_branchMetrics_, m);
}

void PerformanceMetrics::recordTokenSavings(std::size_t raw,
                                            std::size_t compressed) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);

  std::uint64_t r = static_cast<std::uint64_t>(raw);
  std::uint64_t c = static_cast<std::uint64_t>(compressed);
  std::uint64_t saved = r > c ? r - c : 0U;

  s_tokenSavings_.rawTokensTotal += r;
  s_tokenSavings_.compressedTokensTotal += c;
  s_tokenSavings_.savedTokensTotal += saved;
  s_tokenSavings_.sampleCount += 1U;
  s_tokenSavings_.cumulativeSavingsRatio += safeRatio(saved, r);
}

void PerformanceMetrics::recordOverlayReuse(bool reused) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  reused ? ++s_overlayReuseCount_ : ++s_overlayReloadCount_;
}

void PerformanceMetrics::recordSnapshotReuse() {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  ++s_snapshotReuseCount_;
}

void PerformanceMetrics::recordHotSliceLookup(std::size_t hits,
                                              std::size_t lookups) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  s_hotSliceHits_ += hits;
  s_hotSliceLookups_ += lookups;
}

void PerformanceMetrics::recordContextReuse(std::size_t reused,
                                            std::size_t total) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  s_contextReuseHits_ += reused;
  s_contextReuseLookups_ += total;
}

void PerformanceMetrics::recordImpactPredictionAccuracy(const double accuracy) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  pushBounded(s_impactPredictionAccuracySamples_,
              clampDouble(accuracy, 0.0, 1.0));
}

void PerformanceMetrics::recordMemoryGovernance(
    const MemoryGovernanceMetrics& metric) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  s_memoryGovernance_ = metric;
}

void PerformanceMetrics::recordWeightAdjustment(
    const std::string& weightName,
    const double previousValue,
    const double newValue) {
  if (!isEnabled()) return;
  std::scoped_lock lock(s_mutex_);
  s_weightAdjustments_[weightName] = {previousValue, newValue};
  ++s_weightAdjustmentCount_;
}

double PerformanceMetrics::averageTokenSavingsRatio() {
  std::scoped_lock lock(s_mutex_);
  return averageTokenSavingsRatioUnlocked(s_tokenSavings_);
}

nlohmann::ordered_json PerformanceMetrics::report() {
  configureFromEnvironment();
  std::scoped_lock lock(s_mutex_);
  const runtime::GovernorStats governorStats =
      runtime::CPUGovernor::instance().stats();

  const double avgTokenSavingsRatio =
      averageTokenSavingsRatioUnlocked(s_tokenSavings_);
  const double hotSliceHitRatio =
      s_hotSliceLookups_ == 0U
          ? 0.0
          : static_cast<double>(s_hotSliceHits_) /
                static_cast<double>(s_hotSliceLookups_);
  const double contextReuseRate =
      s_contextReuseLookups_ == 0U
          ? 0.0
          : static_cast<double>(s_contextReuseHits_) /
                static_cast<double>(s_contextReuseLookups_);
  const double impactPredictionAccuracy =
      averageSamples(s_impactPredictionAccuracySamples_);
  const std::uint64_t totalLatencyMicros = std::accumulate(
      s_snapshotMetrics_.begin(), s_snapshotMetrics_.end(), std::uint64_t{0U},
      [](const std::uint64_t total, const SnapshotMetrics& metric) {
        return total + metric.durationMicros;
      }) +
      std::accumulate(
          s_contextMetrics_.begin(), s_contextMetrics_.end(), std::uint64_t{0U},
          [](const std::uint64_t total, const ContextMetrics& metric) {
            return total + metric.durationMicros;
          }) +
      std::accumulate(
          s_branchMetrics_.begin(), s_branchMetrics_.end(), std::uint64_t{0U},
          [](const std::uint64_t total, const BranchMetrics& metric) {
            return total + metric.durationMicros;
          });
  const std::size_t totalLatencySamples = s_snapshotMetrics_.size() +
                                          s_contextMetrics_.size() +
                                          s_branchMetrics_.size();
  const double avgLatencyMs =
      totalLatencySamples == 0U
          ? 0.0
          : static_cast<double>(totalLatencyMicros) /
                static_cast<double>(totalLatencySamples) / 1000.0;
  const std::size_t totalOverlayEvents =
      s_overlayReuseCount_ + s_overlayReloadCount_;
  const std::uint64_t totalBranchEvictions = std::accumulate(
      s_branchMetrics_.begin(), s_branchMetrics_.end(), std::uint64_t{0U},
      [](const std::uint64_t total, const BranchMetrics& metric) {
        return total + metric.evictionCount;
      });

  nlohmann::ordered_json snapshot;
  snapshot["avg_creation_time_micros"] =
      averageMicros(s_snapshotMetrics_,
                    [](const SnapshotMetrics& metric) {
                      return metric.durationMicros;
                    });
  snapshot["max_creation_time_micros"] =
      maxMicros(s_snapshotMetrics_,
                [](const SnapshotMetrics& metric) {
                  return metric.durationMicros;
                });
  snapshot["node_count_distribution"] =
      buildNodeCountDistribution(s_snapshotMetrics_);

  nlohmann::ordered_json context;
  context["avg_compression_time_micros"] =
      averageMicros(s_contextMetrics_,
                    [](const ContextMetrics& metric) {
                      return metric.durationMicros;
                    });
  context["avg_tokens_saved"] =
      s_tokenSavings_.sampleCount == 0U
          ? 0.0
          : static_cast<double>(s_tokenSavings_.savedTokensTotal) /
                static_cast<double>(s_tokenSavings_.sampleCount);
  context["compression_ratio"] =
      s_tokenSavings_.rawTokensTotal == 0U
          ? 0.0
          : static_cast<double>(s_tokenSavings_.compressedTokensTotal) /
                static_cast<double>(s_tokenSavings_.rawTokensTotal);
  context["context_reuse_rate"] = contextReuseRate;
  context["hot_slice_hit_rate"] = hotSliceHitRatio;

  nlohmann::ordered_json branch;
  branch["avg_churn_time_micros"] =
      averageMicros(s_branchMetrics_,
                    [](const BranchMetrics& metric) {
                      return metric.durationMicros;
                    });
  branch["eviction_count"] = totalBranchEvictions;
  branch["overlay_reuse_rate"] =
      totalOverlayEvents == 0U
          ? 0.0
          : static_cast<double>(s_overlayReuseCount_) /
                static_cast<double>(totalOverlayEvents);

  nlohmann::ordered_json token;
  token["total_tokens_saved"] = s_tokenSavings_.savedTokensTotal;
  token["avg_savings_percent"] = avgTokenSavingsRatio * 100.0;
  token["estimated_llm_calls_avoided"] =
      s_tokenSavings_.savedTokensTotal / kEstimatedTokensPerCall;

  nlohmann::ordered_json memoryGovernance;
  memoryGovernance["snapshot_version"] = s_memoryGovernance_.snapshotVersion;
  memoryGovernance["branch_id"] = s_memoryGovernance_.branchId;
  memoryGovernance["active_overlay_count"] =
      s_memoryGovernance_.activeOverlayCount;
  memoryGovernance["active_overlay_limit"] =
      s_memoryGovernance_.activeOverlayLimit;
  memoryGovernance["hot_slice_current_size"] =
      s_memoryGovernance_.hotSliceCurrentSize;
  memoryGovernance["hot_slice_target_capacity"] =
      s_memoryGovernance_.hotSliceTargetCapacity;
  memoryGovernance["hot_slice_eviction_count"] =
      s_memoryGovernance_.hotSliceEvictionCount;
  memoryGovernance["hot_slice_recalibration_count"] =
      s_memoryGovernance_.hotSliceRecalibrationCount;
  memoryGovernance["hot_slice_hit_rate"] =
      s_memoryGovernance_.hotSliceHitRate;
  memoryGovernance["context_reuse_rate"] =
      s_memoryGovernance_.contextReuseRate;
  memoryGovernance["token_budget_scale"] =
      s_memoryGovernance_.tokenBudgetScale;
  memoryGovernance["compression_depth"] =
      s_memoryGovernance_.compressionDepth;
  memoryGovernance["pruning_threshold"] =
      s_memoryGovernance_.pruningThreshold;
  memoryGovernance["impact_prediction_accuracy"] =
      s_memoryGovernance_.impactPredictionAccuracy;

  nlohmann::ordered_json reflectiveOptimization;
  reflectiveOptimization["token_savings"] = avgTokenSavingsRatio;
  reflectiveOptimization["context_reuse_rate"] = contextReuseRate;
  reflectiveOptimization["hot_slice_hit_rate"] = hotSliceHitRatio;
  reflectiveOptimization["impact_prediction_accuracy"] =
      impactPredictionAccuracy;
  reflectiveOptimization["compression_efficiency"] = avgTokenSavingsRatio;
  reflectiveOptimization["weight_adjustment_count"] =
      s_weightAdjustmentCount_;
  reflectiveOptimization["weight_adjustments"] =
      nlohmann::ordered_json::array();
  for (const auto& [name, adjustment] : s_weightAdjustments_) {
    nlohmann::ordered_json entry;
    entry["name"] = name;
    entry["previous"] = adjustment.first;
    entry["current"] = adjustment.second;
    reflectiveOptimization["weight_adjustments"].push_back(std::move(entry));
  }

  nlohmann::ordered_json cpuGovernor;
  cpuGovernor["active_workloads"] = governorStats.activeWorkloads;
  cpuGovernor["average_execution_time_ms"] = governorStats.movingAverageMs;
  cpuGovernor["calibration_count"] = governorStats.calibrationCount;
  cpuGovernor["hardware_threads"] = governorStats.hardwareThreads;
  cpuGovernor["idle"] = governorStats.idle;
  cpuGovernor["max_recommended_threads"] =
      governorStats.maxRecommendedThreadCount;
  cpuGovernor["min_recommended_threads"] =
      governorStats.minRecommendedThreadCount;
  cpuGovernor["recommended_threads"] = governorStats.recommendedThreadCount;
  cpuGovernor["workload_count"] = governorStats.workloads.size();

  nlohmann::ordered_json workloads = nlohmann::ordered_json::array();
  for (const auto& [name, workload] : governorStats.workloads) {
    nlohmann::ordered_json entry;
    entry["active_count"] = workload.activeCount;
    entry["average_execution_time_ms"] = workload.averageExecutionMs;
    entry["name"] = name;
    entry["recommended_threads"] = workload.recommendedThreadCount;
    entry["registration_count"] = workload.registrationCount;
    entry["sample_count"] = workload.sampleCount;
    workloads.push_back(std::move(entry));
  }
  cpuGovernor["workloads"] = std::move(workloads);

  nlohmann::ordered_json out;
  out["enabled"] = s_enabled_.load();
  out["snapshot_samples"] = s_snapshotMetrics_.size();
  out["context_samples"] = s_contextMetrics_.size();
  out["branch_samples"] = s_branchMetrics_.size();
  out["overlay_reuse_count"] = s_overlayReuseCount_;
  out["overlay_reload_count"] = s_overlayReloadCount_;
  out["snapshot_reuse_count"] = s_snapshotReuseCount_;
  out["hot_slice_hit_ratio"] = hotSliceHitRatio;
  out["context_reuse_rate"] = contextReuseRate;
  out["impact_prediction_accuracy"] = impactPredictionAccuracy;
  out["snapshot"] = std::move(snapshot);
  out["context"] = std::move(context);
  out["branch"] = std::move(branch);
  out["token"] = std::move(token);
  out["memory_governance"] = std::move(memoryGovernance);
  out["reflective_optimization"] = std::move(reflectiveOptimization);
  out["cpu_governor"] = std::move(cpuGovernor);
  out["avg_token_savings_ratio"] = avgTokenSavingsRatio;
  out["avg_latency_ms"] = avgLatencyMs;
  out["error_rate"] = 0.0;
  return out;
}

void PerformanceMetrics::reset() {
  std::scoped_lock lock(s_mutex_);
  s_snapshotMetrics_.clear();
  s_contextMetrics_.clear();
  s_branchMetrics_.clear();
  s_tokenSavings_ = TokenSavingsMetrics{};
  s_overlayReuseCount_ = 0U;
  s_overlayReloadCount_ = 0U;
  s_snapshotReuseCount_ = 0U;
  s_hotSliceHits_ = 0U;
  s_hotSliceLookups_ = 0U;
  s_contextReuseHits_ = 0U;
  s_contextReuseLookups_ = 0U;
  s_impactPredictionAccuracySamples_.clear();
  s_weightAdjustments_.clear();
  s_weightAdjustmentCount_ = 0U;
  s_memoryGovernance_ = MemoryGovernanceMetrics{};
}

}  // namespace ultra::metrics
