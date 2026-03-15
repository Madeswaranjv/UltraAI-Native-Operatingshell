#pragma once

#include "../memory/EpisodicMemory.h"
#include "../memory/StrategicMemory.h"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ultra::metacognition {

struct MetaCognitiveSignal {
  double stabilityScore{0.0};
  double driftScore{0.0};
  double learningVelocity{0.0};
  bool enterConservativeMode{false};
  bool enterExploratoryMode{false};
};

struct QueryMetrics {
  double stabilityScore{0.0};
  double driftScore{0.0};
  double learningVelocity{0.0};
  std::string predictedNextCommand;
  std::size_t queryTokenBudget{0U};
  std::size_t queryCacheCapacity{0U};
  std::size_t hotSliceCapacity{0U};
  std::size_t branchRetentionHint{0U};
};

class MetaCognitiveOrchestrator {
 public:
  explicit MetaCognitiveOrchestrator() = default;

  static MetaCognitiveOrchestrator& instance();

  [[nodiscard]] MetaCognitiveSignal evaluate(
      const ultra::memory::StrategicMemory& strategic,
      const ultra::memory::EpisodicMemory& episodic) const;

  [[nodiscard]] QueryMetrics recordQuery(const std::string& symbol,
                                         std::uint64_t graphVersion,
                                         std::size_t tokenBudget,
                                         std::size_t queryCacheCapacity,
                                         std::size_t hotSliceCapacity);

  [[nodiscard]] QueryMetrics latestQueryMetrics() const;

 private:
  struct QuerySample {
    std::string symbol;
    std::chrono::steady_clock::time_point timestamp;
    std::uint64_t graphVersion{0U};
  };

  [[nodiscard]] double computeStability(
      const std::vector<ultra::memory::StrategicOutcome>& strategicOutcomes,
      const std::vector<ultra::memory::EpisodicEvent>& episodicEvents) const;

  [[nodiscard]] double computeDrift(
      const std::vector<ultra::memory::StrategicOutcome>& strategicOutcomes,
      const std::vector<ultra::memory::EpisodicEvent>& episodicEvents) const;

  [[nodiscard]] double computeLearningVelocity(
      const std::vector<ultra::memory::StrategicOutcome>& strategicOutcomes,
      const std::vector<ultra::memory::EpisodicEvent>& episodicEvents) const;

  [[nodiscard]] double computeQueryStability(
      const std::deque<QuerySample>& samples) const;
  [[nodiscard]] double computeQueryDrift(
      const std::deque<QuerySample>& samples) const;
  [[nodiscard]] double computeQueryVelocity(
      const std::deque<QuerySample>& samples,
      std::chrono::steady_clock::time_point now) const;

  static constexpr std::size_t kQueryWindow = 32U;
  static constexpr std::chrono::seconds kVelocityWindow{60};

  mutable std::shared_mutex queryMutex_;
  std::deque<QuerySample> recentQueries_;
  QueryMetrics lastQueryMetrics_{};
};

}  // namespace ultra::metacognition






