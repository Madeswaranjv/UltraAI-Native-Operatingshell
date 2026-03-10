#pragma once

#include <string>
#include <vector>
#include <map>

namespace ultra::diff {

/// Report on the potential impact of a set of changes on the whole project.
struct ImpactReport {
  /// Map of modified file to a list of downstream files affected.
  std::map<std::string, std::vector<std::string>> dependencyPropagationMap;

  /// Estimated probability (0.0 - 1.0) of a regression occurring.
  double regressionProbability{0.0};

  /// Measure of how structurally risky the changes are.
  double structuralRiskIndex{0.0};

  /// List of files that are likely to be affected by these changes.
  std::vector<std::string> affectedFiles;

  /// List of files that were analyzed and deemed safe.
  std::vector<std::string> safeFiles;
};

}  // namespace ultra::diff
