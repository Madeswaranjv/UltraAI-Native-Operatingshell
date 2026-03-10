#pragma once

#include "CognitiveState.h"

namespace ultra::core {
class StateManager;
}

namespace ultra::runtime {

class SnapshotPinGuard {
 public:
  SnapshotPinGuard(const core::StateManager& stateManager,
                   const CognitiveState& state);

  void assertCurrent() const;

 private:
  const core::StateManager& stateManager_;
  const CognitiveState& state_;
};

class CognitiveRuntime {
 public:
  explicit CognitiveRuntime(core::StateManager& stateManager) noexcept;

  [[nodiscard]] CognitiveState createState(
      TokenBudget budget,
      const RelevanceProfile& weights = {}) const;

  [[nodiscard]] SnapshotPinGuard pin(const CognitiveState& state) const;

 private:
  core::StateManager& stateManager_;
};

}  // namespace ultra::runtime
