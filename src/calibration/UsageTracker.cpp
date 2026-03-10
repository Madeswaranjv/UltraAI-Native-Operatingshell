#include "UsageTracker.h"

namespace ultra::calibration {

void UsageTracker::record(const std::string& command, const std::vector<std::string>& args) {
  UsageEvent event;
  event.command = command;
  event.args = args;
  event.timestamp = ultra::types::Timestamp::now();
  
  history_.push_back(std::move(event));
  
  if (history_.size() > maxHistory_) {
    history_.erase(history_.begin());
  }
}

std::vector<UsageEvent> UsageTracker::getHistory() const {
  return history_;
}

void UsageTracker::clear() {
  history_.clear();
}

}  // namespace ultra::calibration
