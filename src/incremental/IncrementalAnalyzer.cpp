#include "IncrementalAnalyzer.h"
#include "../graph/DependencyGraph.h"
#include <queue>
#include <unordered_set>
#include <algorithm>

namespace ultra::incremental {

std::vector<std::string> IncrementalAnalyzer::computeRebuildSet(
    const std::vector<std::string>& changedFiles,
    const ultra::graph::DependencyGraph& graph) {
  std::unordered_set<std::string> rebuildSet;
  std::queue<std::string> q;
  for (const std::string& f : changedFiles) {
    if (rebuildSet.insert(f).second) q.push(f);
  }
  while (!q.empty()) {
    std::string n = q.front();
    q.pop();
    for (const std::string& dependent : graph.getDependencies(n)) {
      if (rebuildSet.insert(dependent).second) {
        q.push(dependent);
      }
    }
  }
  std::vector<std::string> out(rebuildSet.begin(), rebuildSet.end());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace ultra::incremental
