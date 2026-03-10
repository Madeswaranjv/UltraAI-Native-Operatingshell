#pragma once

#include "IDecomposer.h"
#include <string>

namespace ultra::orchestration {

/// Default rule-based intent decomposer relying on keyword/pattern matching.
class RuleBasedDecomposer : public IDecomposer {
 public:
  TaskGraph decompose(const std::string& goal) const override;
};

}  // namespace ultra::orchestration
