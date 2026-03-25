#include "CognitiveState.h"

#include "../GraphSnapshot.h"
#include "../../memory/HotSlice.h"

namespace ultra::runtime {

CognitiveState::CognitiveState(const GraphSnapshot& snap,
                               std::shared_ptr<memory::HotSlice> slice,
                               TokenBudget tokenBudget,
                               const RelevanceProfile& profile)
    : snapshot(snap),
      workingSet(std::move(slice)),
      weights(profile),
      budget(tokenBudget),
      branch(snap.branch),
      pinnedVersion(snap.version),
      pinnedHash(snap.deterministicHash()) {}

}  // namespace ultra::runtime