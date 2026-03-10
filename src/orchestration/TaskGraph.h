#pragma once

#include <string>
#include <vector>
#include <map>

namespace ultra::orchestration {

/// Represents a single decomposed sub-task generated from a high-level intent.
struct SubTask {
  std::string taskId;
  std::string description;
  std::vector<std::string> inputs;
  std::vector<std::string> expectedOutputs;
  float estimatedComplexity{1.0f};
};

/// A Directed Acyclic Graph of sub-tasks forming a plan to achieve a goal.
class TaskGraph {
 public:
  void addNode(const SubTask& task);
  void addDependency(const std::string& sourceId, const std::string& targetId);

  SubTask getNode(const std::string& taskId) const;
  
  /// Returns a topologically sorted list of task IDs.
  std::vector<std::string> topologicalOrder() const;

  std::vector<SubTask> getAllNodes() const;

 private:
  std::map<std::string, SubTask> nodes_;
  std::map<std::string, std::vector<std::string>> outboundMap_;
  std::map<std::string, std::vector<std::string>> inboundMap_;
};

}  // namespace ultra::orchestration
