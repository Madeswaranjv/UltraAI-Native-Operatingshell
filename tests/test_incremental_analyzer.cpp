// ============================================================================
// File: tests/incremental/test_incremental_analyzer.cpp
// Tests for IncrementalAnalyzer rebuild set computation
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "incremental/IncrementalAnalyzer.h"
#include "graph/DependencyGraph.h"

using namespace ultra::incremental;
using namespace ultra::graph;

class IncrementalAnalyzerTest : public ::testing::Test {
 protected:
  bool containsString(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
  }
};

TEST_F(IncrementalAnalyzerTest, SingleChangeNoDepends) {
  DependencyGraph graph;
  graph.addNode("file.cpp");
  
  std::vector<std::string> changed{"file.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 1);
  EXPECT_TRUE(containsString(rebuild, "file.cpp"));
}

TEST_F(IncrementalAnalyzerTest, SingleChangePropagates) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  
  std::vector<std::string> changed{"a.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 2);
  EXPECT_TRUE(containsString(rebuild, "a.cpp"));
  EXPECT_TRUE(containsString(rebuild, "b.cpp"));
}

TEST_F(IncrementalAnalyzerTest, TwoLevelPropagation) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addNode("c.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  graph.addDependency("b.cpp", "c.cpp");
  
  std::vector<std::string> changed{"a.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 3);
  EXPECT_TRUE(containsString(rebuild, "a.cpp"));
  EXPECT_TRUE(containsString(rebuild, "b.cpp"));
  EXPECT_TRUE(containsString(rebuild, "c.cpp"));
}

TEST_F(IncrementalAnalyzerTest, ThreeLevelPropagation) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addNode("c.cpp");
  graph.addNode("d.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  graph.addDependency("b.cpp", "c.cpp");
  graph.addDependency("c.cpp", "d.cpp");
  
  std::vector<std::string> changed{"a.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 4);
}

TEST_F(IncrementalAnalyzerTest, MultipleChanges) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addNode("c.cpp");
  graph.addDependency("a.cpp", "c.cpp");
  graph.addDependency("b.cpp", "c.cpp");
  
  std::vector<std::string> changed{"a.cpp", "b.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 3);
  EXPECT_TRUE(containsString(rebuild, "a.cpp"));
  EXPECT_TRUE(containsString(rebuild, "b.cpp"));
  EXPECT_TRUE(containsString(rebuild, "c.cpp"));
}

TEST_F(IncrementalAnalyzerTest, DiamondDependency) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addNode("c.cpp");
  graph.addNode("d.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  graph.addDependency("a.cpp", "c.cpp");
  graph.addDependency("b.cpp", "d.cpp");
  graph.addDependency("c.cpp", "d.cpp");
  
  std::vector<std::string> changed{"a.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 4);
  EXPECT_TRUE(containsString(rebuild, "a.cpp"));
  EXPECT_TRUE(containsString(rebuild, "b.cpp"));
  EXPECT_TRUE(containsString(rebuild, "c.cpp"));
  EXPECT_TRUE(containsString(rebuild, "d.cpp"));
}

TEST_F(IncrementalAnalyzerTest, IsolatedChange) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addNode("c.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  
  std::vector<std::string> changed{"c.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 1);
  EXPECT_TRUE(containsString(rebuild, "c.cpp"));
}

TEST_F(IncrementalAnalyzerTest, NoChanges) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  
  std::vector<std::string> changed;
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 0);
}

TEST_F(IncrementalAnalyzerTest, LargeDependencyGraph) {
  DependencyGraph graph;
  int nodeCount = 30;
  
  for (int i = 0; i < nodeCount; ++i) {
    graph.addNode("file_" + std::to_string(i) + ".cpp");
  }
  
  for (int i = 0; i < nodeCount - 1; ++i) {
    graph.addDependency("file_" + std::to_string(i) + ".cpp",
                        "file_" + std::to_string(i + 1) + ".cpp");
  }
  
  std::vector<std::string> changed{"file_0.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), nodeCount);
}

TEST_F(IncrementalAnalyzerTest, DeterministicRebuildSet) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addNode("c.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  graph.addDependency("b.cpp", "c.cpp");
  
  std::vector<std::string> changed{"a.cpp"};
  
  auto rebuild1 = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  auto rebuild2 = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild1.size(), rebuild2.size());
  for (const auto& node : rebuild1) {
    EXPECT_TRUE(containsString(rebuild2, node));
  }
}

TEST_F(IncrementalAnalyzerTest, NoDuplicateRebuildEntries) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  
  std::vector<std::string> changed{"a.cpp", "a.cpp", "a.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 2);
}

TEST_F(IncrementalAnalyzerTest, CircularDependencyHandling) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addDependency("a.cpp", "b.cpp");
  graph.addDependency("b.cpp", "a.cpp");
  
  std::vector<std::string> changed{"a.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_GE(rebuild.size(), 1);
}

TEST_F(IncrementalAnalyzerTest, BroadDependency) {
  DependencyGraph graph;
  graph.addNode("base.cpp");
  
  for (int i = 0; i < 10; ++i) {
    graph.addNode("user_" + std::to_string(i) + ".cpp");
    graph.addDependency("base.cpp", "user_" + std::to_string(i) + ".cpp");
  }
  
  std::vector<std::string> changed{"base.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 11);
}

TEST_F(IncrementalAnalyzerTest, DeepDependencyChain) {
  DependencyGraph graph;
  int depth = 20;
  
  for (int i = 0; i < depth; ++i) {
    graph.addNode("level_" + std::to_string(i) + ".cpp");
  }
  
  for (int i = 0; i < depth - 1; ++i) {
    graph.addDependency("level_" + std::to_string(i) + ".cpp",
                        "level_" + std::to_string(i + 1) + ".cpp");
  }
  
  std::vector<std::string> changed{"level_0.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), depth);
}

TEST_F(IncrementalAnalyzerTest, MultipleIndependentPaths) {
  DependencyGraph graph;
  graph.addNode("a1.cpp");
  graph.addNode("a2.cpp");
  graph.addNode("a3.cpp");
  graph.addNode("b1.cpp");
  graph.addNode("b2.cpp");
  graph.addNode("b3.cpp");
  
  graph.addDependency("a1.cpp", "a2.cpp");
  graph.addDependency("a2.cpp", "a3.cpp");
  graph.addDependency("b1.cpp", "b2.cpp");
  graph.addDependency("b2.cpp", "b3.cpp");
  
  std::vector<std::string> changed{"a1.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 3);
}

TEST_F(IncrementalAnalyzerTest, ComplexGraph) {
  DependencyGraph graph;
  
  graph.addNode("core.cpp");
  graph.addNode("util1.cpp");
  graph.addNode("util2.cpp");
  graph.addNode("feature_a.cpp");
  graph.addNode("feature_b.cpp");
  graph.addNode("app.cpp");
  
  graph.addDependency("core.cpp", "util1.cpp");
  graph.addDependency("core.cpp", "util2.cpp");
  graph.addDependency("util1.cpp", "feature_a.cpp");
  graph.addDependency("util2.cpp", "feature_b.cpp");
  graph.addDependency("feature_a.cpp", "app.cpp");
  graph.addDependency("feature_b.cpp", "app.cpp");
  
  std::vector<std::string> changed{"core.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 6);
}

TEST_F(IncrementalAnalyzerTest, EmptyGraphChanged) {
  DependencyGraph graph;
  
  std::vector<std::string> changed{"file.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 1);
}

TEST_F(IncrementalAnalyzerTest, NonexistentNodeChanged) {
  DependencyGraph graph;
  graph.addNode("exists.cpp");
  
  std::vector<std::string> changed{"nonexistent.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 1);
  EXPECT_TRUE(containsString(rebuild, "nonexistent.cpp"));
}

TEST_F(IncrementalAnalyzerTest, MiddleOfChainChanged) {
  DependencyGraph graph;
  graph.addNode("a.cpp");
  graph.addNode("b.cpp");
  graph.addNode("c.cpp");
  graph.addNode("d.cpp");
  
  graph.addDependency("a.cpp", "b.cpp");
  graph.addDependency("b.cpp", "c.cpp");
  graph.addDependency("c.cpp", "d.cpp");
  
  std::vector<std::string> changed{"b.cpp"};
  auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
  
  EXPECT_EQ(rebuild.size(), 3);
  EXPECT_TRUE(containsString(rebuild, "b.cpp"));
  EXPECT_TRUE(containsString(rebuild, "c.cpp"));
  EXPECT_TRUE(containsString(rebuild, "d.cpp"));
}

TEST_F(IncrementalAnalyzerTest, Stability) {
  DependencyGraph graph;
  for (int i = 0; i < 10; ++i) {
    graph.addNode("node_" + std::to_string(i) + ".cpp");
  }
  for (int i = 0; i < 9; ++i) {
    graph.addDependency("node_" + std::to_string(i) + ".cpp",
                        "node_" + std::to_string(i + 1) + ".cpp");
  }
  
  std::vector<std::string> changed{"node_0.cpp"};
  
  std::vector<size_t> sizes;
  for (int run = 0; run < 3; ++run) {
    auto rebuild = IncrementalAnalyzer::computeRebuildSet(changed, graph);
    sizes.push_back(rebuild.size());
  }
  
  EXPECT_EQ(sizes[0], sizes[1]);
  EXPECT_EQ(sizes[1], sizes[2]);
}
