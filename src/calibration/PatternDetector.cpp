#include "PatternDetector.h"

namespace ultra::calibration {

std::map<std::string, std::map<std::string, int>> PatternDetector::computeTransitions(const std::vector<UsageEvent>& history) const {
  std::map<std::string, std::map<std::string, int>> transitions;
  if (history.size() < 2) return transitions;

  for (std::size_t i = 0; i < history.size() - 1; ++i) {
    const auto& current = history[i].command;
    const auto& next = history[i + 1].command;
    transitions[current][next]++;
  }

  return transitions;
}

std::string PatternDetector::predictNextCommand(const std::vector<UsageEvent>& history, const std::string& currentCommand) const {
  auto transitions = computeTransitions(history);
  
  auto it = transitions.find(currentCommand);
  if (it == transitions.end() || it->second.empty()) {
    return "";
  }

  std::string mostLikely;
  int maxCount = 0;

  for (const auto& [nextCmd, count] : it->second) {
    if (count > maxCount) {
      maxCount = count;
      mostLikely = nextCmd;
    }
  }

  // Basic threshold to avoid noise: pattern must appear at least 3 times
  if (maxCount >= 3) {
    return mostLikely;
  }

  return "";
}

}  // namespace ultra::calibration
