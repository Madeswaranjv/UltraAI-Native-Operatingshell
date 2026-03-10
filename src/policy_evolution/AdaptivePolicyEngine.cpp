#include "AdaptivePolicyEngine.h"

#include <algorithm>
#include <cmath>

namespace ultra::policy_evolution {

namespace {

double clampBias(const double value) {
  return std::clamp(value, -0.2, 0.2);
}

double modeRatio(
    const std::deque<metacognition::MetaCognitiveSignal>& signals,
    const bool conservativeMode) {
  if (signals.empty()) {
    return 0.0;
  }

  std::size_t count = 0U;
  for (const auto& signal : signals) {
    if (conservativeMode ? signal.enterConservativeMode
                         : signal.enterExploratoryMode) {
      ++count;
    }
  }

  return static_cast<double>(count) /
         static_cast<double>(signals.size());
}

double driftTrendDelta(
    const std::deque<metacognition::MetaCognitiveSignal>& signals) {
  if (signals.size() < 4U) {
    return 0.0;
  }

  const std::size_t mid = signals.size() / 2U;
  if (mid == 0U || mid >= signals.size()) {
    return 0.0;
  }

  double olderAverage = 0.0;
  for (std::size_t index = 0U; index < mid; ++index) {
    olderAverage += signals[index].driftScore;
  }
  olderAverage /= static_cast<double>(mid);

  double recentAverage = 0.0;
  for (std::size_t index = mid; index < signals.size(); ++index) {
    recentAverage += signals[index].driftScore;
  }
  recentAverage /= static_cast<double>(signals.size() - mid);

  return recentAverage - olderAverage;
}

}  // namespace

EvolutionAdjustment AdaptivePolicyEngine::update(
    const ultra::metacognition::MetaCognitiveSignal& signal) {
  recentSignals_.push_back(signal);
  if (recentSignals_.size() > kWindowSize) {
    recentSignals_.pop_front();
  }

  EvolutionAdjustment adjustment;
  if (recentSignals_.empty()) {
    return adjustment;
  }

  const double conservativeRatio = modeRatio(recentSignals_, true);
  const double exploratoryRatio = modeRatio(recentSignals_, false);

  if (conservativeRatio > 0.60) {
    const double persistenceStrength =
        (conservativeRatio - 0.60) / 0.40;
    adjustment.determinismBiasShift =
        clampBias(persistenceStrength * 0.20);
  }

  if (exploratoryRatio > 0.60) {
    const double persistenceStrength =
        (exploratoryRatio - 0.60) / 0.40;
    adjustment.explorationBiasShift =
        clampBias(persistenceStrength * 0.20);
  }

  const double driftDelta = driftTrendDelta(recentSignals_);
  if (driftDelta > 0.0) {
    const double persistenceFactor = computeTrendPersistence();
    const double scaledShift =
        driftDelta * (0.10 + (0.10 * persistenceFactor));
    adjustment.riskBiasShift = clampBias(scaledShift);
  }

  adjustment.riskBiasShift = clampBias(adjustment.riskBiasShift);
  adjustment.determinismBiasShift = clampBias(adjustment.determinismBiasShift);
  adjustment.explorationBiasShift = clampBias(adjustment.explorationBiasShift);
  return adjustment;
}

double AdaptivePolicyEngine::computeTrendPersistence() const {
  if (recentSignals_.empty()) {
    return 0.0;
  }
  return std::max(modeRatio(recentSignals_, true),
                  modeRatio(recentSignals_, false));
}

}  // namespace ultra::policy_evolution
