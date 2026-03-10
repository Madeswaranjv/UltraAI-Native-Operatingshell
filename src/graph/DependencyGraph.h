#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::graph {

class DependencyGraph {
 public:
  void addNode(const std::string& file);
  void addEdge(const std::string& from, const std::string& to);
  void addDependency(const std::string& from, const std::string& to);
  void removeEdge(const std::string& from, const std::string& to);
  void removeDependency(const std::string& from, const std::string& to);
  void removeNode(const std::string& file);
  void updateNode(const std::string& file,
                  const std::vector<std::string>& dependencies);

  [[nodiscard]] bool hasCycle() const;
  [[nodiscard]] std::vector<std::string> topologicalSort() const;
  [[nodiscard]] std::vector<std::string> getDependencies(
      const std::string& file) const;
  [[nodiscard]] std::vector<std::string> getNodes() const;

  [[nodiscard]] std::size_t nodeCount() const noexcept;
  [[nodiscard]] std::size_t edgeCount() const noexcept;

 private:
  enum class VisitState { Unvisited, Visiting, Visited };
  bool hasCycleFrom(const std::string& node,
                    std::unordered_map<std::string, VisitState>& state) const;

  std::unordered_map<std::string, std::vector<std::string>> adjacency_;
  std::size_t edgeCount_{0};
};

}  // namespace ultra::graph
