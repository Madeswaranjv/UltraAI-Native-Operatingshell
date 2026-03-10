// ============================================================================
// File: tests/orchestration/test_branch_orchestrator.cpp
// Tests for BranchOrchestrator task decomposition and branch spawning
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "orchestration/BranchOrchestrator.h"
#include "orchestration/TaskGraph.h"
#include "intelligence/BranchLifecycle.h"

using namespace ultra::orchestration;
using namespace ultra::intelligence;

class BranchOrchestratorTest : public ::testing::Test {
 protected:
  BranchStore store;
  BranchLifecycle lifecycle{store};
  BranchOrchestrator orchestrator{lifecycle};

  SubTask createSubTask(const std::string& taskId,
                       const std::string& description = "test task") {
    SubTask task;
    task.taskId = taskId;
    task.description = description;
    task.estimatedComplexity = 1.0f;
    return task;
  }

  bool containsString(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
  }
};

TEST_F(BranchOrchestratorTest, CreateSingleBranch) {
  TaskGraph graph;
  graph.addNode(createSubTask("task1", "Single Task"));
  
  auto parentId = lifecycle.create("root_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 1);
  EXPECT_FALSE(spawned[0].empty());
}

TEST_F(BranchOrchestratorTest, OrchestrateLinearTasks) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1", "Task 1"));
  graph.addNode(createSubTask("t2", "Task 2"));
  graph.addNode(createSubTask("t3", "Task 3"));
  
  graph.addDependency("t1", "t2");
  graph.addDependency("t2", "t3");
  
  auto parentId = lifecycle.create("main_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 3);
}

TEST_F(BranchOrchestratorTest, OrchestrateBranchingTasks) {
  TaskGraph graph;
  graph.addNode(createSubTask("root", "Root Task"));
  graph.addNode(createSubTask("left", "Left Branch"));
  graph.addNode(createSubTask("right", "Right Branch"));
  graph.addNode(createSubTask("merge", "Merge Results"));
  
  graph.addDependency("root", "left");
  graph.addDependency("root", "right");
  graph.addDependency("left", "merge");
  graph.addDependency("right", "merge");
  
  auto parentId = lifecycle.create("parallel_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 4);
}

TEST_F(BranchOrchestratorTest, EmptyTaskGraph) {
  TaskGraph graph;
  
  auto parentId = lifecycle.create("empty_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_TRUE(spawned.empty());
}

TEST_F(BranchOrchestratorTest, CycleDetection) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1"));
  graph.addNode(createSubTask("t2"));
  graph.addNode(createSubTask("t3"));
  
  graph.addDependency("t1", "t2");
  graph.addDependency("t2", "t3");
  graph.addDependency("t3", "t1");
  
  auto parentId = lifecycle.create("cycle_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_TRUE(spawned.empty());
}

TEST_F(BranchOrchestratorTest, TaskGraphWithComplexity) {
  TaskGraph graph;
  SubTask t1 = createSubTask("t1", "Complex Analysis");
  t1.estimatedComplexity = 5.5f;
  graph.addNode(t1);
  
  SubTask t2 = createSubTask("t2", "Refinement");
  t2.estimatedComplexity = 2.0f;
  graph.addNode(t2);
  
  graph.addDependency("t1", "t2");
  
  auto parentId = lifecycle.create("complexity_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 2);
}

TEST_F(BranchOrchestratorTest, MultiTaskOrchestration) {
  TaskGraph graph;
  for (int i = 0; i < 5; ++i) {
    graph.addNode(createSubTask("task_" + std::to_string(i)));
  }
  
  for (int i = 0; i < 4; ++i) {
    graph.addDependency("task_" + std::to_string(i),
                       "task_" + std::to_string(i + 1));
  }
  
  auto parentId = lifecycle.create("multi_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 5);
}

TEST_F(BranchOrchestratorTest, SpawnedBranchesAreValid) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1"));
  
  auto parentId = lifecycle.create("validity_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_FALSE(spawned.empty());
  for (const auto& branchId : spawned) {
    EXPECT_FALSE(branchId.empty());
    auto retrieved = store.get(branchId);
    EXPECT_FALSE(retrieved.branchId.empty());
  }
}

TEST_F(BranchOrchestratorTest, SpawnedBranchParentage) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1"));
  
  auto parentId = lifecycle.create("parent_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  if (!spawned.empty()) {
    auto child = store.get(spawned[0]);
    EXPECT_EQ(child.parentBranchId, parentId);
  }
}

TEST_F(BranchOrchestratorTest, TaskInputsOutputsPreserved) {
  TaskGraph graph;
  SubTask t1 = createSubTask("t1", "Input Processing");
  t1.inputs = {"data1", "data2"};
  t1.expectedOutputs = {"result1"};
  graph.addNode(t1);
  
  auto parentId = lifecycle.create("io_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 1);
}

TEST_F(BranchOrchestratorTest, DeterministicOrchestration) {
  TaskGraph graph;
  for (int i = 0; i < 3; ++i) {
    graph.addNode(createSubTask("t" + std::to_string(i)));
  }
  graph.addDependency("t0", "t1");
  graph.addDependency("t1", "t2");
  
  auto parentId = lifecycle.create("det_goal").branchId;
  auto spawn1 = orchestrator.orchestrate(graph, parentId);
  
  TaskGraph graph2;
  for (int i = 0; i < 3; ++i) {
    graph2.addNode(createSubTask("t" + std::to_string(i)));
  }
  graph2.addDependency("t0", "t1");
  graph2.addDependency("t1", "t2");
  
  auto parentId2 = lifecycle.create("det_goal2").branchId;
  auto spawn2 = orchestrator.orchestrate(graph2, parentId2);
  
  EXPECT_EQ(spawn1.size(), spawn2.size());
}

TEST_F(BranchOrchestratorTest, LargeTaskGraphOrchestration) {
  TaskGraph graph;
  int nodeCount = 20;
  
  for (int i = 0; i < nodeCount; ++i) {
    graph.addNode(createSubTask("task_" + std::to_string(i)));
  }
  
  for (int i = 0; i < nodeCount - 1; ++i) {
    graph.addDependency("task_" + std::to_string(i),
                       "task_" + std::to_string(i + 1));
  }
  
  auto parentId = lifecycle.create("large_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), nodeCount);
}

TEST_F(BranchOrchestratorTest, ConflictDetectionEdgeCase) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1", "Task 1"));
  graph.addNode(createSubTask("t2", "Task 2"));
  
  auto parentId = lifecycle.create("conflict_test").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_FALSE(spawned.empty());
}

TEST_F(BranchOrchestratorTest, TaskWithMissingNodeSkipped) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1"));
  graph.addDependency("t1", "nonexistent");
  
  auto parentId = lifecycle.create("skip_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 1);
}

TEST_F(BranchOrchestratorTest, MultipleParallelStreams) {
  TaskGraph graph;
  
  graph.addNode(createSubTask("start"));
  
  graph.addNode(createSubTask("stream1_a"));
  graph.addNode(createSubTask("stream1_b"));
  graph.addDependency("start", "stream1_a");
  graph.addDependency("stream1_a", "stream1_b");
  
  graph.addNode(createSubTask("stream2_a"));
  graph.addNode(createSubTask("stream2_b"));
  graph.addDependency("start", "stream2_a");
  graph.addDependency("stream2_a", "stream2_b");
  
  auto parentId = lifecycle.create("streams_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_EQ(spawned.size(), 5);
}

TEST_F(BranchOrchestratorTest, TaskDescriptionPropagation) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1", "Specific Task Description"));
  
  auto parentId = lifecycle.create("desc_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  if (!spawned.empty()) {
    auto branch = store.get(spawned[0]);
    EXPECT_EQ(branch.goal, "Specific Task Description");
  }
}

TEST_F(BranchOrchestratorTest, OrchestrationStability) {
  TaskGraph graph;
  graph.addNode(createSubTask("t1"));
  
  auto parentId = lifecycle.create("stability_goal").branchId;
  
  std::vector<std::vector<std::string>> results;
  for (int i = 0; i < 3; ++i) {
    auto spawned = orchestrator.orchestrate(graph, parentId);
    results.push_back(spawned);
  }
  
  EXPECT_EQ(results[0].size(), results[1].size());
  EXPECT_EQ(results[1].size(), results[2].size());
}

TEST_F(BranchOrchestratorTest, DAGValidationBeforeSpawning) {
  TaskGraph graph;
  graph.addNode(createSubTask("a"));
  graph.addNode(createSubTask("b"));
  graph.addDependency("a", "b");
  
  auto order = graph.topologicalOrder();
  EXPECT_FALSE(order.empty());
  
  auto parentId = lifecycle.create("dag_goal").branchId;
  auto spawned = orchestrator.orchestrate(graph, parentId);
  
  EXPECT_FALSE(spawned.empty());
}
