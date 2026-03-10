#pragma once

#include <string>
#include <vector>
#include "../types/Timestamp.h"

namespace ultra::calibration {

/// Represents a single recorded user/system action for pattern mining.
struct UsageEvent {
  std::string command;
  std::vector<std::string> args;
  ultra::types::Timestamp timestamp;
};

/// Tracks sequential commands to map typical operational journeys.
class UsageTracker {
 public:
  void record(const std::string& command, const std::vector<std::string>& args);
  std::vector<UsageEvent> getHistory() const;
  void clear();

 private:
  std::vector<UsageEvent> history_;
  const std::size_t maxHistory_{1000};
};

}  // namespace ultra::calibration
