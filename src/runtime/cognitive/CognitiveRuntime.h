#pragma once

#include "CognitiveState.h"
#include "tools/ToolRegistry.h"
#include "../CPUGovernor.h"
#include "../governance/Policy.h"
#include "../intent/Intent.h"

#include <cstddef>
#include <string>

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

struct GovernanceSignal {
  bool idle{false};
  std::size_t activeWorkloads{0U};
  std::size_t recommendedThreads{1U};
  std::size_t hardwareThreads{1U};
  double movingAverageMs{0.0};
};

struct CognitiveLoopResult {
  bool ok{false};
  std::string verifyStatus;
  std::string confidence;
  std::string output;
  std::string errorMessage;
};

class CognitiveRuntime {
 public:
  explicit CognitiveRuntime(core::StateManager& stateManager) noexcept;

  [[nodiscard]] CognitiveState createState(
      TokenBudget budget,
      const RelevanceProfile& weights = {}) const;

  [[nodiscard]] SnapshotPinGuard pin(const CognitiveState& state) const;
  [[nodiscard]] const cognitive::tools::ToolRegistry& toolRegistry() const noexcept;
  [[nodiscard]] GovernanceSignal currentGovernanceSignal() const;
  [[nodiscard]] CognitiveLoopResult run(
      const intent::Intent& intentValue,
      const governance::Policy& policy);

 private:
  core::StateManager& stateManager_;
  cognitive::tools::ToolRegistry toolRegistry_{};
};

}  // namespace ultra::runtime

