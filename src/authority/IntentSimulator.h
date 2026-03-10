#pragma once

#include "../diff/BranchSemanticDiff.h"

#include <cstddef>
#include <filesystem>

namespace ultra::authority {

struct AuthorityIntentRequest;

struct SimulatedIntentResult {
  diff::semantic::BranchDiffReport diffReport;
  std::size_t impactDepth{0U};
  std::size_t publicApiChanges{0U};
};

class IntentSimulator {
 public:
  explicit IntentSimulator(
      std::filesystem::path projectRoot = std::filesystem::current_path());

  [[nodiscard]] SimulatedIntentResult simulate(
      const AuthorityIntentRequest& request) const;

 private:
  std::filesystem::path projectRoot_;
};

}  // namespace ultra::authority
