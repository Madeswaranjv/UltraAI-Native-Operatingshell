#pragma once

#include "../GraphSnapshot.h"
#include "../../memory/HotSlice.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace ultra::runtime {

using BranchID = BranchId;
using NodeID = std::string;
using TokenBudget = std::size_t;

struct RelevanceProfile {
  double recencyWeight{0.35};
  double centralityWeight{0.25};
  double usageWeight{0.20};
  double impactWeight{0.20};
};

struct CognitiveState {
  GraphSnapshot snapshot;
  std::shared_ptr<memory::HotSlice> workingSet;
  RelevanceProfile weights;
  TokenBudget budget{0U};
  BranchID branch{BranchID::nil()};
  std::uint64_t pinnedVersion{0U};
  std::string pinnedHash;

  CognitiveState(const GraphSnapshot& snap,
                 std::shared_ptr<memory::HotSlice> slice,
                 TokenBudget tokenBudget,
                 const RelevanceProfile& profile = {});
};

}  // namespace ultra::runtime
