#include "BranchIdGenerator.h"
#include "../ai/Hashing.h"
//BranchIdGenerator.cpp
#include <string>

namespace ultra::intelligence {

std::string BranchIdGenerator::generate(const std::string& parentId,
                                        const std::string& goal,
                                        uint64_t deterministicCounter) const {
  std::string input = parentId + "|" + goal + "|" + std::to_string(deterministicCounter);
  auto hash = ultra::ai::sha256OfString(input);
  return ultra::ai::hashToHex(hash);
}

}  // namespace ultra::intelligence
