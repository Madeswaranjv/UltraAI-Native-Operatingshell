#pragma once

#include <string>
#include <vector>

namespace ultra::graph {
class DependencyGraph;
}

namespace ultra::incremental {

class IncrementalAnalyzer {
 public:
  static std::vector<std::string> computeRebuildSet(
      const std::vector<std::string>& changedFiles,
      const ultra::graph::DependencyGraph& graph);
};

}  // namespace ultra::incremental
