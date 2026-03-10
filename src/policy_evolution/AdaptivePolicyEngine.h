#pragma once

#include "../metacognition/MetaCognitiveOrchestrator.h"

#include <cstddef>
#include <deque>

namespace ultra::policy_evolution {

struct EvolutionAdjustment {
  double riskBiasShift{0.0};
  double determinismBiasShift{0.0};
  double explorationBiasShift{0.0};
};

class AdaptivePolicyEngine {
 public:
  AdaptivePolicyEngine() = default;

  [[nodiscard]] EvolutionAdjustment update(
      const ultra::metacognition::MetaCognitiveSignal& signal);

 private:
  [[nodiscard]] double computeTrendPersistence() const;

  std::deque<ultra::metacognition::MetaCognitiveSignal> recentSignals_;
  static constexpr std::size_t kWindowSize = 20U;
};

}  // namespace ultra::policy_evolution
