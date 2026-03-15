#include "CognitiveState.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace ultra::runtime {

CognitiveState::CognitiveState(const GraphSnapshot& snap,
                               std::shared_ptr<memory::HotSlice> slice,
                               const TokenBudget tokenBudget,
                               const RelevanceProfile& profile)
    : snapshot(snap),
      workingSet(std::move(slice)),
      weights(profile),
      budget(tokenBudget),
      branch(snap.branch),
      pinnedVersion(snap.version),
      pinnedHash(snap.deterministicHash()) {
  if (!workingSet) {
    throw std::runtime_error("CognitiveState requires a working hot slice.");
  }
  if (!snapshot.graph) {
    throw std::runtime_error("CognitiveState requires a non-empty graph snapshot.");
  }
  if (budget == 0U) {
    throw std::runtime_error("CognitiveState requires a non-zero token budget.");
  }
  if (pinnedVersion == 0U) {
    throw std::runtime_error("CognitiveState requires a non-zero snapshot version.");
  }
  if (pinnedHash.empty()) {
    throw std::runtime_error("CognitiveState requires a deterministic snapshot hash.");
  }
  const bool invalidWeights =
      !std::isfinite(weights.recencyWeight) || !std::isfinite(weights.centralityWeight) ||
      !std::isfinite(weights.usageWeight) || !std::isfinite(weights.impactWeight) ||
      weights.recencyWeight < 0.0 || weights.centralityWeight < 0.0 ||
      weights.usageWeight < 0.0 || weights.impactWeight < 0.0;
  if (invalidWeights) {
    throw std::runtime_error("CognitiveState relevance profile contains invalid weights.");
  }
  if ((weights.recencyWeight + weights.centralityWeight + weights.usageWeight +
       weights.impactWeight) <= 0.0) {
    throw std::runtime_error("CognitiveState relevance profile must have positive total weight.");
  }
  workingSet->syncVersions(pinnedVersion);
}

}  // namespace ultra::runtime
