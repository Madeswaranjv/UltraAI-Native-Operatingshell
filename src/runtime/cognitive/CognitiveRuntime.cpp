#include "CognitiveRuntime.h"

#include "../../core/state_manager.h"

#include <stdexcept>

namespace ultra::runtime {

SnapshotPinGuard::SnapshotPinGuard(const core::StateManager& stateManager,
                                   const CognitiveState& state)
    : stateManager_(stateManager), state_(state) {
  assertCurrent();
}

void SnapshotPinGuard::assertCurrent() const {
  if (state_.snapshot.version != state_.pinnedVersion) {
    throw std::runtime_error(
        "Snapshot pin mismatch: pinned version diverges from state snapshot.");
  }
  stateManager_.ensureSnapshotCurrent(state_.snapshot);
  const std::string livePinnedHash = state_.snapshot.deterministicHash();
  if (livePinnedHash != state_.pinnedHash) {
    throw std::runtime_error(
        "Snapshot pin mismatch: deterministic hash diverges from pinned hash.");
  }
}

CognitiveRuntime::CognitiveRuntime(core::StateManager& stateManager) noexcept
    : stateManager_(stateManager) {}

CognitiveState CognitiveRuntime::createState(
    const TokenBudget budget,
    const RelevanceProfile& weights) const {
  return stateManager_.createCognitiveState(
      static_cast<std::size_t>(budget), weights);
}

SnapshotPinGuard CognitiveRuntime::pin(const CognitiveState& state) const {
  return SnapshotPinGuard(stateManager_, state);
}

}  // namespace ultra::runtime
