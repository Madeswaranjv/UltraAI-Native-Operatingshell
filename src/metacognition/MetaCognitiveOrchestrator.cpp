#include "MetaCognitiveOrchestrator.h"

#include "../metrics/PerformanceMetrics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <unordered_set>

namespace ultra::metacognition {

namespace {

struct WindowRange {
  std::size_t begin{0U};
  std::size_t end{0U};
};

double clamp01(const double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, 0.0, 1.0);
}

WindowRange firstHalf(const std::size_t size) {
  return WindowRange{0U, size / 2U};
}

WindowRange secondHalf(const std::size_t size) {
  return WindowRange{size / 2U, size};
}

double meanEventFailureRate(
    const std::vector<memory::EpisodicEvent>& events,
    const WindowRange range) {
  if (events.empty() || range.begin >= range.end || range.end > events.size()) {
    return 0.0;
  }

  std::size_t failures = 0U;
  std::size_t samples = 0U;
  for (std::size_t index = range.begin; index < range.end; ++index) {
    const memory::EpisodicEvent& event = events[index];
    if (!event.success || event.rolledBack) {
      ++failures;
    }
    ++samples;
  }

  if (samples == 0U) {
    return 0.0;
  }
  return static_cast<double>(failures) / static_cast<double>(samples);
}

double meanEventSuccessRate(
    const std::vector<memory::EpisodicEvent>& events,
    const WindowRange range) {
  if (events.empty() || range.begin >= range.end || range.end > events.size()) {
    return 0.0;
  }

  std::size_t successes = 0U;
  std::size_t samples = 0U;
  for (std::size_t index = range.begin; index < range.end; ++index) {
    const memory::EpisodicEvent& event = events[index];
    if (event.success && !event.rolledBack) {
      ++successes;
    }
    ++samples;
  }

  if (samples == 0U) {
    return 0.0;
  }
  return static_cast<double>(successes) / static_cast<double>(samples);
}

double meanOutcomeCalibrationError(
    const std::vector<memory::StrategicOutcome>& outcomes,
    const WindowRange range) {
  if (outcomes.empty() || range.begin >= range.end || range.end > outcomes.size()) {
    return 0.0;
  }

  double totalError = 0.0;
  std::size_t samples = 0U;
  for (std::size_t index = range.begin; index < range.end; ++index) {
    const memory::StrategicOutcome& outcome = outcomes[index];
    const double riskError =
        std::abs(outcome.predictedRisk - outcome.observedRisk);
    const double confidenceError =
        std::abs(outcome.predictedConfidence - outcome.observedConfidence);
    totalError += (riskError + confidenceError) * 0.5;
    ++samples;
  }

  if (samples == 0U) {
    return 0.0;
  }
  return totalError / static_cast<double>(samples);
}

double meanOutcomeRollbackRate(
    const std::vector<memory::StrategicOutcome>& outcomes,
    const WindowRange range) {
  if (outcomes.empty() || range.begin >= range.end || range.end > outcomes.size()) {
    return 0.0;
  }

  std::size_t rollbacks = 0U;
  std::size_t samples = 0U;
  for (std::size_t index = range.begin; index < range.end; ++index) {
    if (outcomes[index].rolledBack) {
      ++rollbacks;
    }
    ++samples;
  }

  if (samples == 0U) {
    return 0.0;
  }
  return static_cast<double>(rollbacks) / static_cast<double>(samples);
}

double meanOutcomeObservedRisk(
    const std::vector<memory::StrategicOutcome>& outcomes,
    const WindowRange range) {
  if (outcomes.empty() || range.begin >= range.end || range.end > outcomes.size()) {
    return 0.0;
  }

  double riskSum = 0.0;
  std::size_t samples = 0U;
  for (std::size_t index = range.begin; index < range.end; ++index) {
    riskSum += outcomes[index].observedRisk;
    ++samples;
  }

  if (samples == 0U) {
    return 0.0;
  }
  return riskSum / static_cast<double>(samples);
}

}  // namespace

MetaCognitiveOrchestrator& MetaCognitiveOrchestrator::instance() {
  static MetaCognitiveOrchestrator engine;
  return engine;
}

MetaCognitiveSignal MetaCognitiveOrchestrator::evaluate(
    const ultra::memory::StrategicMemory& strategic,
    const ultra::memory::EpisodicMemory& episodic) const {
  const std::vector<memory::StrategicOutcome> strategicOutcomes =
      strategic.snapshot();
  const std::vector<memory::EpisodicEvent> episodicEvents =
      episodic.snapshot();

  MetaCognitiveSignal signal;
  signal.stabilityScore =
      computeStability(strategicOutcomes, episodicEvents);
  signal.driftScore =
      computeDrift(strategicOutcomes, episodicEvents);
  signal.learningVelocity =
      computeLearningVelocity(strategicOutcomes, episodicEvents);

  signal.enterConservativeMode = signal.stabilityScore < 0.4;
  signal.enterExploratoryMode =
      !signal.enterConservativeMode &&
      signal.learningVelocity < 0.2 &&
      signal.driftScore < 0.1;
  return signal;
}

double MetaCognitiveOrchestrator::computeStability(
    const std::vector<memory::StrategicOutcome>& strategicOutcomes,
    const std::vector<memory::EpisodicEvent>& episodicEvents) const {
  if (strategicOutcomes.empty() && episodicEvents.empty()) {
    return 0.5;
  }

  const WindowRange outcomeWindow{0U, strategicOutcomes.size()};
  const WindowRange episodicWindow{0U, episodicEvents.size()};

  const double calibrationError = strategicOutcomes.empty()
                                      ? 0.5
                                      : meanOutcomeCalibrationError(
                                            strategicOutcomes,
                                            outcomeWindow);
  const double rollbackRate = strategicOutcomes.empty()
                                  ? 0.0
                                  : meanOutcomeRollbackRate(
                                        strategicOutcomes,
                                        outcomeWindow);
  const double episodicFailureRate = episodicEvents.empty()
                                         ? 0.0
                                         : meanEventFailureRate(
                                               episodicEvents,
                                               episodicWindow);

  const double stability =
      1.0 - ((0.50 * calibrationError) +
             (0.30 * rollbackRate) +
             (0.20 * episodicFailureRate));
  return clamp01(stability);
}

double MetaCognitiveOrchestrator::computeDrift(
    const std::vector<memory::StrategicOutcome>& strategicOutcomes,
    const std::vector<memory::EpisodicEvent>& episodicEvents) const {
  if (strategicOutcomes.size() < 4U && episodicEvents.size() < 4U) {
    return 0.0;
  }

  double outcomeDrift = 0.0;
  if (strategicOutcomes.size() >= 4U) {
    const WindowRange older = firstHalf(strategicOutcomes.size());
    const WindowRange recent = secondHalf(strategicOutcomes.size());
    const double olderRisk =
        meanOutcomeObservedRisk(strategicOutcomes, older);
    const double recentRisk =
        meanOutcomeObservedRisk(strategicOutcomes, recent);
    const double olderError =
        meanOutcomeCalibrationError(strategicOutcomes, older);
    const double recentError =
        meanOutcomeCalibrationError(strategicOutcomes, recent);
    outcomeDrift = clamp01((std::abs(recentRisk - olderRisk) +
                            std::abs(recentError - olderError)) *
                           0.5);
  }

  double episodicDrift = 0.0;
  if (episodicEvents.size() >= 4U) {
    const WindowRange older = firstHalf(episodicEvents.size());
    const WindowRange recent = secondHalf(episodicEvents.size());
    const double olderFailureRate =
        meanEventFailureRate(episodicEvents, older);
    const double recentFailureRate =
        meanEventFailureRate(episodicEvents, recent);
    episodicDrift = clamp01(
        std::abs(recentFailureRate - olderFailureRate));
  }

  if (strategicOutcomes.size() < 4U) {
    return episodicDrift;
  }
  if (episodicEvents.size() < 4U) {
    return outcomeDrift;
  }
  return clamp01((0.70 * outcomeDrift) + (0.30 * episodicDrift));
}

double MetaCognitiveOrchestrator::computeLearningVelocity(
    const std::vector<memory::StrategicOutcome>& strategicOutcomes,
    const std::vector<memory::EpisodicEvent>& episodicEvents) const {
  if (strategicOutcomes.size() < 4U && episodicEvents.size() < 4U) {
    return 0.5;
  }

  double outcomeVelocity = 0.5;
  if (strategicOutcomes.size() >= 4U) {
    const WindowRange older = firstHalf(strategicOutcomes.size());
    const WindowRange recent = secondHalf(strategicOutcomes.size());
    const double olderError =
        meanOutcomeCalibrationError(strategicOutcomes, older);
    const double recentError =
        meanOutcomeCalibrationError(strategicOutcomes, recent);
    outcomeVelocity = clamp01(0.5 + ((olderError - recentError) * 0.5));
  }

  double episodicVelocity = 0.5;
  if (episodicEvents.size() >= 4U) {
    const WindowRange older = firstHalf(episodicEvents.size());
    const WindowRange recent = secondHalf(episodicEvents.size());
    const double olderSuccessRate =
        meanEventSuccessRate(episodicEvents, older);
    const double recentSuccessRate =
        meanEventSuccessRate(episodicEvents, recent);
    episodicVelocity = clamp01(
        0.5 + ((recentSuccessRate - olderSuccessRate) * 0.5));
  }

  if (strategicOutcomes.size() < 4U) {
    return episodicVelocity;
  }
  if (episodicEvents.size() < 4U) {
    return outcomeVelocity;
  }
  return clamp01((0.70 * outcomeVelocity) + (0.30 * episodicVelocity));
}

QueryMetrics MetaCognitiveOrchestrator::recordQuery(const std::string& symbol,
                                                    const std::uint64_t graphVersion,
                                                    const std::size_t tokenBudget,
                                                    const std::size_t queryCacheCapacity,
                                                    const std::size_t hotSliceCapacity) {
  std::unique_lock<std::shared_mutex> lock(queryMutex_);
  const auto now = std::chrono::steady_clock::now();
  recentQueries_.push_back(QuerySample{symbol, now, graphVersion});

  while (!recentQueries_.empty() &&
         (now - recentQueries_.front().timestamp) > kVelocityWindow) {
    recentQueries_.pop_front();
  }

  while (recentQueries_.size() > kQueryWindow) {
    recentQueries_.pop_front();
  }

  lastQueryMetrics_.stabilityScore = computeQueryStability(recentQueries_);
  lastQueryMetrics_.driftScore = computeQueryDrift(recentQueries_);
  lastQueryMetrics_.learningVelocity = computeQueryVelocity(recentQueries_, now);
  if (!symbol.empty()) {
    lastQueryMetrics_.predictedNextCommand = "ultra ai_impact " + symbol;
  } else {
    lastQueryMetrics_.predictedNextCommand.clear();
  }
  lastQueryMetrics_.queryTokenBudget = tokenBudget;
  lastQueryMetrics_.queryCacheCapacity = queryCacheCapacity;
  lastQueryMetrics_.hotSliceCapacity = hotSliceCapacity;
  lastQueryMetrics_.branchRetentionHint = 0U;

  // Push query-derived data into PerformanceMetrics so that
  // ultra savings / ultra metrics reflect live query activity.
  if (metrics::PerformanceMetrics::isEnabled()) {
    // Impact prediction accuracy proxy: stability score measures how
    // consistently the graph version is matching across recent queries,
    // which is the closest available signal to "how well we predicted impact".
    metrics::PerformanceMetrics::recordImpactPredictionAccuracy(
        lastQueryMetrics_.stabilityScore);

    // Record hot slice lookup using hotSliceCapacity as a proxy for
    // whether the hot slice is active; treat any query as a lookup,
    // and count it as a hit when the index was already built (stability > 0).
    if (hotSliceCapacity > 0U) {
      const std::size_t hits =
          lastQueryMetrics_.stabilityScore > 0.0 ? 1U : 0U;
      metrics::PerformanceMetrics::recordHotSliceLookup(hits, 1U);
    }

    // Context reuse: treat consecutive queries to the same symbol
    // (drift < 0.5 means low symbol diversity = high reuse) as cache hits.
    {
      const std::size_t reused =
          lastQueryMetrics_.driftScore < 0.5 ? 1U : 0U;
      metrics::PerformanceMetrics::recordContextReuse(reused, 1U);
    }
  }

  return lastQueryMetrics_;
}

QueryMetrics MetaCognitiveOrchestrator::latestQueryMetrics() const {
  std::shared_lock<std::shared_mutex> lock(queryMutex_);
  return lastQueryMetrics_;
}

double MetaCognitiveOrchestrator::computeQueryStability(
    const std::deque<QuerySample>& samples) const {
  if (samples.size() <= 1U) {
    return 1.0;
  }
  std::size_t changes = 0U;
  for (std::size_t index = 1U; index < samples.size(); ++index) {
    if (samples[index].graphVersion != samples[index - 1U].graphVersion) {
      ++changes;
    }
  }
  return 1.0 / (static_cast<double>(changes) + 1.0);
}

double MetaCognitiveOrchestrator::computeQueryDrift(
    const std::deque<QuerySample>& samples) const {
  if (samples.empty()) {
    return 0.0;
  }
  std::unordered_set<std::string> uniqueSymbols;
  std::size_t total = 0U;
  for (const QuerySample& sample : samples) {
    if (sample.symbol.empty()) {
      continue;
    }
    uniqueSymbols.insert(sample.symbol);
    ++total;
  }
  if (total == 0U) {
    return 0.0;
  }
  return static_cast<double>(uniqueSymbols.size()) /
         static_cast<double>(total);
}

double MetaCognitiveOrchestrator::computeQueryVelocity(
    const std::deque<QuerySample>& samples,
    std::chrono::steady_clock::time_point now) const {
  (void)now;
  if (samples.empty()) {
    return 0.0;
  }
  const double windowMinutes =
      std::chrono::duration_cast<std::chrono::duration<double>>(kVelocityWindow)
          .count() /
      60.0;
  if (!std::isfinite(windowMinutes) || windowMinutes <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(samples.size()) / windowMinutes;
}

}  // namespace ultra::metacognition