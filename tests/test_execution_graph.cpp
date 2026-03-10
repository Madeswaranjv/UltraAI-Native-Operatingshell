// ============================================================================
// File: tests/execution/test_execution_graph.cpp
// Tests for ExecutionGraph execution step DAG
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "intelligence/ExecutionGraph.h"
#include "intelligence/ExecutionNode.h"
#include "types/Timestamp.h"

using namespace ultra::intelligence;

class ExecutionGraphTest : public ::testing::Test {
 protected:
  ExecutionGraph graph;

  ExecutionNode createNode(const std::string& nodeId,
                          const std::string& branchId = "branch1",
                          NodeStatus status = NodeStatus::Pending,
                          const std::string& action = "test_action") {
    ExecutionNode node;
    node.nodeId = nodeId;
    node.branchId = branchId;
    node.action = action;
    node.status = status;
    node.timestamp = ultra::types::Timestamp::now();
    node.durationMs = 0;
    return node;
  }

  bool containsNode(const std::vector<std::string>& nodes, const std::string& nodeId) {
    return std::find(nodes.begin(), nodes.end(), nodeId) != nodes.end();
  }
};

TEST_F(ExecutionGraphTest, AddExecutionNode) {
  ExecutionNode node = createNode("node1");
  graph.addNode(node);
  
  ExecutionNode retrieved = graph.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(ExecutionGraphTest, RetrieveNonexistentNode) {
  ExecutionNode retrieved = graph.getNode("nonexistent");
  EXPECT_EQ(retrieved.nodeId, "");
}

TEST_F(ExecutionGraphTest, AddDependency) {
  graph.addNode(createNode("n1"));
  graph.addNode(createNode("n2"));
  
  graph.addDependency("n1", "n2");
  
  auto deps = graph.getDependencies("n2");
  EXPECT_TRUE(containsNode(deps, "n1"));
}

TEST_F(ExecutionGraphTest, MultipleDependencies) {
  graph.addNode(createNode("n1"));
  graph.addNode(createNode("n2"));
  graph.addNode(createNode("n3"));
  
  graph.addDependency("n1", "n3");
  graph.addDependency("n2", "n3");
  
  auto deps = graph.getDependencies("n3");
  EXPECT_EQ(deps.size(), 2);
}

TEST_F(ExecutionGraphTest, DAGValidationSimple) {
  graph.addNode(createNode("n1"));
  graph.addNode(createNode("n2"));
  graph.addDependency("n1", "n2");
  
  auto order = graph.topologicalOrder();
  EXPECT_EQ(order.size(), 2);
  EXPECT_EQ(order[0], "n1");
  EXPECT_EQ(order[1], "n2");
}

TEST_F(ExecutionGraphTest, CycleDetection) {
  graph.addNode(createNode("n1"));
  graph.addNode(createNode("n2"));
  graph.addNode(createNode("n3"));
  
  graph.addDependency("n1", "n2");
  graph.addDependency("n2", "n3");
  graph.addDependency("n3", "n1");
  
  auto order = graph.topologicalOrder();
  EXPECT_TRUE(order.empty());
}

TEST_F(ExecutionGraphTest, SelfCycleDetection) {
  graph.addNode(createNode("n1"));
  graph.addDependency("n1", "n1");
  
  auto order = graph.topologicalOrder();
  EXPECT_TRUE(order.empty());
}

TEST_F(ExecutionGraphTest, ExecutionOrderCorrectness) {
  graph.addNode(createNode("a"));
  graph.addNode(createNode("b"));
  graph.addNode(createNode("c"));
  graph.addNode(createNode("d"));
  
  graph.addDependency("a", "b");
  graph.addDependency("a", "c");
  graph.addDependency("b", "d");
  graph.addDependency("c", "d");
  
  auto order = graph.topologicalOrder();
  ASSERT_EQ(order.size(), 4);
  
  EXPECT_EQ(order[0], "a");
  EXPECT_EQ(order[3], "d");
}

TEST_F(ExecutionGraphTest, MultipleIndependentBranches) {
  graph.addNode(createNode("n1", "branch1"));
  graph.addNode(createNode("n2", "branch1"));
  graph.addNode(createNode("n3", "branch2"));
  graph.addNode(createNode("n4", "branch2"));
  
  graph.addDependency("n1", "n2");
  graph.addDependency("n3", "n4");
  
  auto order = graph.topologicalOrder();
  EXPECT_EQ(order.size(), 4);
}

TEST_F(ExecutionGraphTest, DiamondDependency) {
  graph.addNode(createNode("top"));
  graph.addNode(createNode("left"));
  graph.addNode(createNode("right"));
  graph.addNode(createNode("bottom"));
  
  graph.addDependency("top", "left");
  graph.addDependency("top", "right");
  graph.addDependency("left", "bottom");
  graph.addDependency("right", "bottom");
  
  auto order = graph.topologicalOrder();
  ASSERT_EQ(order.size(), 4);
  EXPECT_EQ(order[0], "top");
  EXPECT_EQ(order[3], "bottom");
}

TEST_F(ExecutionGraphTest, LargeExecutionGraph30Nodes) {
  for (int i = 0; i < 30; ++i) {
    graph.addNode(createNode("node_" + std::to_string(i)));
  }
  
  for (int i = 0; i < 29; ++i) {
    graph.addDependency("node_" + std::to_string(i), "node_" + std::to_string(i + 1));
  }
  
  auto order = graph.topologicalOrder();
  EXPECT_EQ(order.size(), 30);
}

TEST_F(ExecutionGraphTest, DeterministicExecutionOrdering) {
  graph.addNode(createNode("a"));
  graph.addNode(createNode("b"));
  graph.addNode(createNode("c"));
  graph.addDependency("a", "b");
  graph.addDependency("a", "c");
  
  auto order1 = graph.topologicalOrder();
  auto order2 = graph.topologicalOrder();
  
  EXPECT_EQ(order1, order2);
}

TEST_F(ExecutionGraphTest, DuplicateNodeHandling) {
  ExecutionNode n1 = createNode("dup");
  graph.addNode(n1);
  
  ExecutionNode n2 = createNode("dup");
  n2.action = "different_action";
  graph.addNode(n2);
  
  ExecutionNode retrieved = graph.getNode("dup");
  EXPECT_EQ(retrieved.action, "different_action");
}

TEST_F(ExecutionGraphTest, DuplicateEdgeHandling) {
  graph.addNode(createNode("n1"));
  graph.addNode(createNode("n2"));
  
  graph.addDependency("n1", "n2");
  graph.addDependency("n1", "n2");
  
  auto deps = graph.getDependencies("n2");
  EXPECT_EQ(std::count(deps.begin(), deps.end(), "n1"), 1);
}

TEST_F(ExecutionGraphTest, InvalidNodeReference) {
  graph.addNode(createNode("n1"));
  graph.addDependency("n1", "nonexistent");
  
  auto order = graph.topologicalOrder();
  EXPECT_EQ(order.size(), 1);
  EXPECT_EQ(order[0], "n1");
}

TEST_F(ExecutionGraphTest, GetActivePath) {
  graph.addNode(createNode("n1", "b1", NodeStatus::Pending));
  graph.addNode(createNode("n2", "b1", NodeStatus::Running));
  graph.addNode(createNode("n3", "b1", NodeStatus::Running));
  graph.addNode(createNode("n4", "b1", NodeStatus::Success));
  
  auto active = graph.getActivePath();
  EXPECT_EQ(active.size(), 2);
  EXPECT_TRUE(containsNode(active, "n2"));
  EXPECT_TRUE(containsNode(active, "n3"));
}

TEST_F(ExecutionGraphTest, GetNodesByBranch) {
  graph.addNode(createNode("n1", "branch1"));
  graph.addNode(createNode("n2", "branch1"));
  graph.addNode(createNode("n3", "branch2"));
  
  auto branch1Nodes = graph.getNodesByBranch("branch1");
  EXPECT_EQ(branch1Nodes.size(), 2);
  EXPECT_EQ(branch1Nodes[0].branchId, "branch1");
}

TEST_F(ExecutionGraphTest, GetNodesByBranchEmpty) {
  graph.addNode(createNode("n1", "branch1"));
  
  auto branch2Nodes = graph.getNodesByBranch("branch2");
  EXPECT_TRUE(branch2Nodes.empty());
}

TEST_F(ExecutionGraphTest, ClearGraph) {
  graph.addNode(createNode("n1"));
  graph.addNode(createNode("n2"));
  
  graph.clear();
  
  ExecutionNode retrieved = graph.getNode("n1");
  EXPECT_EQ(retrieved.nodeId, "");
}

TEST_F(ExecutionGraphTest, ComplexDependencyChain) {
  for (int i = 0; i < 5; ++i) {
    graph.addNode(createNode("n" + std::to_string(i)));
  }
  
  for (int i = 0; i < 4; ++i) {
    graph.addDependency("n" + std::to_string(i), "n" + std::to_string(i + 1));
  }
  
  auto order = graph.topologicalOrder();
  ASSERT_EQ(order.size(), 5);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(order[i], "n" + std::to_string(i));
  }
}

TEST_F(ExecutionGraphTest, MultipleStartNodes) {
  graph.addNode(createNode("start1"));
  graph.addNode(createNode("start2"));
  graph.addNode(createNode("end"));
  
  graph.addDependency("start1", "end");
  graph.addDependency("start2", "end");
  
  auto order = graph.topologicalOrder();
  ASSERT_EQ(order.size(), 3);
  EXPECT_EQ(order[2], "end");
}

TEST_F(ExecutionGraphTest, NodeStatusTracking) {
  ExecutionNode node = createNode("n1", "b1", NodeStatus::Running);
  graph.addNode(node);
  
  ExecutionNode retrieved = graph.getNode("n1");
  EXPECT_EQ(retrieved.status, NodeStatus::Running);
}

TEST_F(ExecutionGraphTest, BranchIdConsistency) {
  ExecutionNode node = createNode("n1", "specific_branch");
  graph.addNode(node);
  
  ExecutionNode retrieved = graph.getNode("n1");
  EXPECT_EQ(retrieved.branchId, "specific_branch");
}

TEST_F(ExecutionGraphTest, WideDependencyTree) {
  graph.addNode(createNode("root"));
  for (int i = 0; i < 10; ++i) {
    std::string child = "child_" + std::to_string(i);
    graph.addNode(createNode(child));
    graph.addDependency("root", child);
  }
  
  auto order = graph.topologicalOrder();
  EXPECT_EQ(order.size(), 11);
  EXPECT_EQ(order[0], "root");
}
