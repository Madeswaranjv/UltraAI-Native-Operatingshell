#pragma once

#include "ExecutionNode.h"
#include <map>
#include <string>
#include <vector>
#include <set>

namespace ultra::intelligence {

/// A Directed Acyclic Graph mapping out the discrete execution steps of AI reasoning.
class ExecutionGraph {
 public:
  /// Add or update an execution node.
  void addNode(const ExecutionNode& node);

  /// Get a specific node by ID.
  ExecutionNode getNode(const std::string& nodeId) const;

  /// Define a dependency: 'targetId' must run after 'sourceId'.
  void addDependency(const std::string& sourceId, const std::string& targetId);

  /// Retrieves the current active sequence of nodes executing.
  std::vector<std::string> getActivePath() const;

  /// Retrieves all nodes associated with a particular branch ID.
  std::vector<ExecutionNode> getNodesByBranch(const std::string& branchId) const;

  /// Returns a valid topological execution order, or empty if a cycle exists.
  std::vector<std::string> topologicalOrder() const;

  /// Fetch all immediate dependencies of a given node.
  std::vector<std::string> getDependencies(const std::string& nodeId) const;

  void clear();

 private:
  std::map<std::string, ExecutionNode> nodes_;
  
  // Maps a node ID to a list of node IDs that depend on it.
  std::map<std::string, std::vector<std::string>> outboundMap_;
  
  // Maps a node ID to a list of node IDs it depends on.
  std::map<std::string, std::vector<std::string>> inboundMap_;
};

}  // namespace ultra::intelligence
