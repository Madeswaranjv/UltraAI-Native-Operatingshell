#pragma once

#include "../memory/EpisodicMemory.h"
#include "../memory/StrategicMemory.h"

#include <vector>

namespace ultra::metacognition {

struct MetaCognitiveSignal {
  double stabilityScore{0.0};
  double driftScore{0.0};
  double learningVelocity{0.0};
  bool enterConservativeMode{false};
  bool enterExploratoryMode{false};
};

class MetaCognitiveOrchestrator {
 public:
  explicit MetaCognitiveOrchestrator() = default;

  [[nodiscard]] MetaCognitiveSignal evaluate(
      const ultra::memory::StrategicMemory& strategic,
      const ultra::memory::EpisodicMemory& episodic) const;

 private:
  [[nodiscard]] double computeStability(
      const std::vector<ultra::memory::StrategicOutcome>& strategicOutcomes,
      const std::vector<ultra::memory::EpisodicEvent>& episodicEvents) const;

  [[nodiscard]] double computeDrift(
      const std::vector<ultra::memory::StrategicOutcome>& strategicOutcomes,
      const std::vector<ultra::memory::EpisodicEvent>& episodicEvents) const;

  [[nodiscard]] double computeLearningVelocity(
      const std::vector<ultra::memory::StrategicOutcome>& strategicOutcomes,
      const std::vector<ultra::memory::EpisodicEvent>& episodicEvents) const;
};

}  // namespace ultra::metacognition
