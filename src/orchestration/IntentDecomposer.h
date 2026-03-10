#pragma once

#include "IDecomposer.h"
#include "../runtime/intent/Intent.h"

#include <memory>
#include <string>

namespace ultra::orchestration {

/// High-level engine that uses an underlying decomposer algorithm
/// to map natural language intents to actionable TaskGraphs.
class IntentDecomposer {
 public:
  /// Initialize with a specific algorithm, defaults to RuleBasedDecomposer
  explicit IntentDecomposer(std::unique_ptr<IDecomposer> strategy = nullptr);

  /// Convert goal into an executable Directed Acyclic Graph.
  TaskGraph decompose(const std::string& goal) const;
  TaskGraph decompose(const ultra::runtime::intent::Intent& intent) const;

 private:
  std::unique_ptr<IDecomposer> strategy_;
};

}  // namespace ultra::orchestration
