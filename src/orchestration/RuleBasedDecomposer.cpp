#include "RuleBasedDecomposer.h"

#include <algorithm>
#include <cctype>

namespace ultra::orchestration {

namespace {

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
  return s;
}

} // namespace

TaskGraph RuleBasedDecomposer::decompose(const std::string& goal) const {
  TaskGraph graph;
  std::string lowerGoal = toLower(goal);

  // Heuristic: If it asks for an analysis, scanning, or diffing
  if (lowerGoal.find("analyze") != std::string::npos ||
      lowerGoal.find("risk") != std::string::npos ||
      lowerGoal.find("diff") != std::string::npos) {
      
    SubTask scan{"scan_nodes", "Scan the codebase for all target files", {}, {"file_list"}, 1.0f};
    SubTask index{"build_graph", "Parse AST and build dependency graph", {"file_list"}, {"active_graph"}, 2.0f};
    SubTask context{"extract_context", "Extract execution context from active graph", {"active_graph"}, {"context_snapshot"}, 1.5f};
    SubTask diff{"compute_diff", "Compute semantic drift against baseline", {"context_snapshot"}, {"symbol_deltas"}, 3.0f};
    SubTask score{"risk_score", "Quantify regression and risk probability", {"symbol_deltas"}, {"risk_report"}, 2.0f};

    graph.addNode(scan);
    graph.addNode(index);
    graph.addNode(context);
    graph.addNode(diff);
    graph.addNode(score);

    graph.addDependency("scan_nodes", "build_graph");
    graph.addDependency("build_graph", "extract_context");
    graph.addDependency("extract_context", "compute_diff");
    graph.addDependency("compute_diff", "risk_score");
    
    return graph;
  }

  // Fallback: A single generic goal step
  SubTask generic{"execute_goal", "Execute generic single-step intent", {}, {"result_payload"}, 1.0f};
  graph.addNode(generic);
  
  return graph;
}

}  // namespace ultra::orchestration
