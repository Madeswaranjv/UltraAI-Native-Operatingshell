#pragma once
// BranchIdGenerator.h

#include <string>
#include <cstdint>

namespace ultra::intelligence {

class BranchIdGenerator {
 public:
  // Generate deterministic branch id using parentId, goal and a deterministic counter
  std::string generate(const std::string& parentId, const std::string& goal, uint64_t deterministicCounter) const;
};

}  // namespace ultra::intelligence
