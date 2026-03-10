#pragma once
// BranchEvictionPolicy.h

#include <string>
#include <vector>

namespace ultra::intelligence {

class BranchStore;

class BranchEvictionPolicy {
 public:
  // Select a deterministic eviction candidate from the store. Returns empty string if none found.
  std::string selectEvictionCandidate(
      const BranchStore& store,
      const std::vector<std::string>& protectedBranchIds = {},
      bool activeOnly = false) const;
};

}  // namespace ultra::intelligence
