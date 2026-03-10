#include "StrategicMemory.h"

#include "../calibration/WeightTuner.h"

#include <external/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <mutex>

namespace ultra::memory {

namespace {

double clampDouble(double value,
                   double minValue,
                   double maxValue) {
  if (!std::isfinite(value)) return minValue;
  return std::clamp(value, minValue, maxValue);
}

bool lessOutcome(const StrategicOutcome& a,
                 const StrategicOutcome& b) {
  if (a.version != b.version) return a.version < b.version;
  if (a.category != b.category) return a.category < b.category;
  return a.subject < b.subject;
}

}  // namespace

StrategicMemory::StrategicMemory(std::size_t retentionLimit)
    : retentionLimit_(std::max<std::size_t>(1U, retentionLimit)) {}


// =====================================================
// RECORD OUTCOME
// =====================================================

void StrategicMemory::recordOutcome(
    const StrategicOutcome& outcome) {

  std::unique_lock lock(mutex_);

  outcomes_.push_back(outcome);
  std::sort(outcomes_.begin(), outcomes_.end(), lessOutcome);

  while (outcomes_.size() > retentionLimit_) {
    outcomes_.erase(outcomes_.begin());
  }

  if (outcome.category == "intent") {
    ++intentAttempts_;
    if (outcome.success) ++intentSuccesses_;
  }

  if (outcome.category == "merge" ||
      outcome.category == "governance") {
    ++mergeAttempts_;
    if (outcome.success) ++mergeSuccesses_;
  }

  if (outcome.rolledBack) ++rollbackCount_;

  if (outcome.estimatedTokens > 0U) {
    const std::size_t saved =
        outcome.estimatedTokens > outcome.compressedTokens
            ? outcome.estimatedTokens - outcome.compressedTokens
            : 0U;

    cumulativeCompressionEfficiency_ +=
        static_cast<double>(saved) /
        static_cast<double>(outcome.estimatedTokens);

    ++compressionSamples_;
  }

  cumulativeRiskError_ +=
      std::abs(outcome.predictedRisk -
               outcome.observedRisk);
  ++riskSamples_;

  cumulativeConfidenceError_ +=
      std::abs(outcome.predictedConfidence -
               outcome.observedConfidence);
  ++confidenceSamples_;
}


// =====================================================
// POLICY ADJUSTMENTS
// =====================================================

PolicyAdjustments
StrategicMemory::getPolicyAdjustments(
    const PerformanceSnapshot* snapshot) const {

  std::shared_lock lock(mutex_);

  const double intentSuccessRate =
      intentAttempts_ == 0
          ? 0.5
          : static_cast<double>(intentSuccesses_) /
                static_cast<double>(intentAttempts_);

  const double mergeSuccessRate =
      mergeAttempts_ == 0
          ? 0.5
          : static_cast<double>(mergeSuccesses_) /
                static_cast<double>(mergeAttempts_);

  const double rollbackRate =
      intentAttempts_ == 0
          ? 0.0
          : static_cast<double>(rollbackCount_) /
                static_cast<double>(intentAttempts_);

  const double riskError =
      riskSamples_ == 0
          ? 0.0
          : cumulativeRiskError_ /
                static_cast<double>(riskSamples_);

  const double confidenceError =
      confidenceSamples_ == 0
          ? 0.0
          : cumulativeConfidenceError_ /
                static_cast<double>(confidenceSamples_);

  double externalSavings = 0.0;
  double hotSliceHitRate = 0.0;
  double contextReuseRate = 0.0;
  double compressionRatio = 1.0;
  double overlayReuseRate = 0.0;
  double impactPredictionAccuracy = clampDouble(1.0 - riskError, 0.0, 1.0);
  double latencyPenalty = 0.0;
  double externalErrorRate = 0.0;

  if (snapshot != nullptr) {
    externalSavings = snapshot->avgTokenSavingsRatio;
    hotSliceHitRate = clampDouble(snapshot->hotSliceHitRate, 0.0, 1.0);
    contextReuseRate = clampDouble(snapshot->contextReuseRate, 0.0, 1.0);
    compressionRatio = clampDouble(snapshot->compressionRatio, 0.0, 1.0);
    overlayReuseRate = clampDouble(snapshot->overlayReuseRate, 0.0, 1.0);
    if (snapshot->impactPredictionAccuracy > 0.0) {
      impactPredictionAccuracy =
          clampDouble(snapshot->impactPredictionAccuracy, 0.0, 1.0);
    }
    latencyPenalty =
        clampDouble(snapshot->avgLatencyMs / 250.0, 0.0, 1.0);
    externalErrorRate = clampDouble(snapshot->errorRate, 0.0, 1.0);
  }

  const double internalSavings =
      compressionSamples_ == 0
          ? externalSavings
          : clampDouble(
                cumulativeCompressionEfficiency_ /
                    static_cast<double>(compressionSamples_),
                0.0, 1.0);
  if (snapshot == nullptr) {
    compressionRatio = clampDouble(1.0 - internalSavings, 0.0, 1.0);
  }
  if (snapshot == nullptr || contextReuseRate == 0.0) {
    contextReuseRate = hotSliceHitRate;
  }
  const double impactActivity =
      intentAttempts_ == 0U
          ? 0.0
          : clampDouble(static_cast<double>(riskSamples_) /
                            static_cast<double>(intentAttempts_),
                        0.0, 2.0) /
                2.0;

  PolicyAdjustments adjustments;

  adjustments.riskThresholdDelta =
      clampDouble((mergeSuccessRate - 0.5) * 0.30 -
                      (riskError * 0.50),
                  -0.25, 0.25);

  adjustments.determinismFloorDelta =
      clampDouble((intentSuccessRate - 0.5) * 0.20 -
                      (rollbackRate * 0.30) -
                      (confidenceError * 0.20),
                  -0.20, 0.20);

  adjustments.tokenBudgetScale =
      clampDouble(1.0 - (internalSavings * 0.20) +
                      (riskError * 0.10) + (latencyPenalty * 0.05),
                  0.70, 1.00);

  adjustments.hotSliceCapacityScale =
      clampDouble(1.0 + ((0.45 - hotSliceHitRate) * 0.35) +
                      ((0.50 - contextReuseRate) * 0.15) +
                      (latencyPenalty * 0.10),
                  0.75, 1.35);

  adjustments.pruningThreshold =
      clampDouble(0.10 + (compressionRatio * 0.25) +
                      (std::max(0.0, 0.40 - hotSliceHitRate) * 0.20),
                  0.05, 0.65);

  adjustments.compressionDepth = 1U;
  if (compressionRatio > 0.70 || internalSavings < 0.20) {
    adjustments.compressionDepth += 1U;
  }
  if (hotSliceHitRate < 0.35 || contextReuseRate < 0.35) {
    adjustments.compressionDepth += 1U;
  }
  adjustments.compressionDepth =
      std::clamp<std::size_t>(adjustments.compressionDepth, 1U, 3U);
  adjustments.hotSliceHitRate = hotSliceHitRate;
  adjustments.contextReuseRate = contextReuseRate;
  adjustments.compressionRatio = compressionRatio;
  adjustments.impactPredictionAccuracy = impactPredictionAccuracy;

  adjustments.weightSignals["determinism_weight"] =
      static_cast<float>(
          clampDouble((intentSuccessRate - 0.5) -
                          (rollbackRate * 0.50) -
                          (confidenceError * 0.50),
                      -1.0, 1.0));

  adjustments.weightSignals["merge_weight"] =
      static_cast<float>(
          clampDouble((mergeSuccessRate - 0.5) * 1.5,
                      -1.0, 1.0));

  adjustments.weightSignals["risk_weight"] =
      static_cast<float>(
          clampDouble((riskError * 1.5) - 0.5,
                      -1.0, 1.0));

  adjustments.weightSignals["token_weight"] =
      static_cast<float>(
          clampDouble((internalSavings - 0.5) * 1.5,
                      -1.0, 1.0));

  adjustments.weightSignals["recency_weight"] =
      static_cast<float>(
          clampDouble((rollbackRate * 0.60) + (externalErrorRate * 0.40) -
                          contextReuseRate,
                      -1.0, 1.0));

  adjustments.weightSignals["centrality_weight"] =
      static_cast<float>(
          clampDouble((impactActivity - 0.50) + (overlayReuseRate - 0.50),
                      -1.0, 1.0));

  adjustments.weightSignals["usage_weight"] =
      static_cast<float>(
          clampDouble(((hotSliceHitRate - 0.50) * 1.5) +
                          ((contextReuseRate - 0.50) * 0.5),
                      -1.0, 1.0));

  adjustments.weightSignals["impact_weight"] =
      static_cast<float>(
          clampDouble((0.60 - impactPredictionAccuracy) +
                          ((0.50 - hotSliceHitRate) * 0.25),
                      -1.0, 1.0));

  adjustments.weightSignals["dependency_depth_weight"] =
      static_cast<float>(
          clampDouble((compressionRatio - 0.50) + (latencyPenalty * 0.30),
                      -1.0, 1.0));

  adjustments.weightSignals["module_proximity_weight"] =
      static_cast<float>(
          clampDouble((contextReuseRate - 0.50) +
                          ((mergeSuccessRate - 0.50) * 0.50),
                      -1.0, 1.0));

  return adjustments;
}


// =====================================================
// PERSIST
// =====================================================

bool StrategicMemory::persistTuningState(
    const std::filesystem::path& path) const {

  std::shared_lock lock(mutex_);

  nlohmann::ordered_json payload;
  payload["schema_version"] = kSchemaVersion;
  payload["intent_attempts"] = intentAttempts_;
  payload["intent_successes"] = intentSuccesses_;
  payload["merge_attempts"] = mergeAttempts_;
  payload["merge_successes"] = mergeSuccesses_;
  payload["rollback_count"] = rollbackCount_;
  payload["cumulative_compression_efficiency"] =
      cumulativeCompressionEfficiency_;
  payload["compression_samples"] = compressionSamples_;
  payload["cumulative_risk_error"] = cumulativeRiskError_;
  payload["risk_samples"] = riskSamples_;
  payload["cumulative_confidence_error"] =
      cumulativeConfidenceError_;
  payload["confidence_samples"] = confidenceSamples_;
  payload["outcomes"] = nlohmann::ordered_json::array();

  for (const auto& outcome : outcomes_) {
    nlohmann::ordered_json item;
    item["version"] = outcome.version;
    item["category"] = outcome.category;
    item["subject"] = outcome.subject;
    item["success"] = outcome.success;
    item["rolled_back"] = outcome.rolledBack;
    item["predicted_risk"] = outcome.predictedRisk;
    item["observed_risk"] = outcome.observedRisk;
    item["predicted_confidence"] = outcome.predictedConfidence;
    item["observed_confidence"] = outcome.observedConfidence;
    item["estimated_tokens"] = outcome.estimatedTokens;
    item["compressed_tokens"] = outcome.compressedTokens;
    payload["outcomes"].push_back(std::move(item));
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;

  out << payload.dump(2);
  return static_cast<bool>(out);
}


// =====================================================
// LOAD
// =====================================================

bool StrategicMemory::loadTuningState(
    const std::filesystem::path& path) {

  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  nlohmann::json payload;
  try {
    in >> payload;
  } catch (...) {
    return false;
  }

  if (!payload.is_object() ||
      payload.value("schema_version", 0U) != kSchemaVersion)
    return false;

  std::unique_lock lock(mutex_);

  outcomes_.clear();

  for (const auto& item : payload["outcomes"]) {
    StrategicOutcome o;
    o.version = item.value("version", 0U);
    o.category = item.value("category", "");
    o.subject = item.value("subject", "");
    o.success = item.value("success", false);
    o.rolledBack = item.value("rolled_back", false);
    o.predictedRisk = item.value("predicted_risk", 0.0);
    o.observedRisk = item.value("observed_risk", 0.0);
    o.predictedConfidence = item.value("predicted_confidence", 0.0);
    o.observedConfidence = item.value("observed_confidence", 0.0);
    o.estimatedTokens = item.value("estimated_tokens", 0U);
    o.compressedTokens = item.value("compressed_tokens", 0U);
    outcomes_.push_back(o);
  }

  intentAttempts_ = payload.value("intent_attempts", 0U);
  intentSuccesses_ = payload.value("intent_successes", 0U);
  mergeAttempts_ = payload.value("merge_attempts", 0U);
  mergeSuccesses_ = payload.value("merge_successes", 0U);
  rollbackCount_ = payload.value("rollback_count", 0U);
  cumulativeCompressionEfficiency_ =
      payload.value("cumulative_compression_efficiency", 0.0);
  compressionSamples_ =
      payload.value("compression_samples", 0U);
  cumulativeRiskError_ =
      payload.value("cumulative_risk_error", 0.0);
  riskSamples_ =
      payload.value("risk_samples", 0U);
  cumulativeConfidenceError_ =
      payload.value("cumulative_confidence_error", 0.0);
  confidenceSamples_ =
      payload.value("confidence_samples", 0U);

  return true;
}


// =====================================================

void StrategicMemory::applyWeightTuning(
    calibration::WeightTuner& tuner,
    const PerformanceSnapshot* snapshot) const {

  const PolicyAdjustments adj =
      getPolicyAdjustments(snapshot);
  (void)tuner.tuneBatchSmoothed(adj.weightSignals);
}

std::vector<StrategicOutcome>
StrategicMemory::snapshot() const {
  std::shared_lock lock(mutex_);
  return outcomes_;
}

}  // namespace ultra::memory
