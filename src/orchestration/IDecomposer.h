#pragma once

#include "TaskGraph.h"
#include <string>

namespace ultra::orchestration {

/// Interface for algorithms that convert a high-level intent into a plan (TaskGraph).
class IDecomposer {
 public:
  virtual ~IDecomposer() = default;

  /// Decompose a high-level goal string into an actionable TaskGraph DAG.
  virtual TaskGraph decompose(const std::string& goal) const = 0;
};

}  // namespace ultra::orchestration
