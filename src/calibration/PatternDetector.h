#pragma once

#include "UsageTracker.h"
#include <string>
#include <vector>
#include <map>

namespace ultra::calibration {

/// Detects statistically significant sequential execution patterns from history.
class PatternDetector {
 public:
  /// Given a history of events, identify the command most likely to follow the query.
  /// Returns empty string if no pattern is statistically reliable.
  std::string predictNextCommand(const std::vector<UsageEvent>& history, const std::string& currentCommand) const;

 private:
  // Detects sequence pairs (A -> B)
  std::map<std::string, std::map<std::string, int>> computeTransitions(const std::vector<UsageEvent>& history) const;
};

}  // namespace ultra::calibration
