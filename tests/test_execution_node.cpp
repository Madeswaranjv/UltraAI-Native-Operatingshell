// ============================================================================
// File: tests/execution/test_execution_node.cpp
// Tests for ExecutionNode struct
// ============================================================================

#include <gtest/gtest.h>
#include "intelligence/ExecutionNode.h"
#include "types/Timestamp.h"

using namespace ultra::intelligence;

class ExecutionNodeTest : public ::testing::Test {
 protected:
  ExecutionNode createDefaultNode() {
    ExecutionNode node;
    node.nodeId = "test_node";
    node.branchId = "test_branch";
    node.action = "test_action";
    node.status = NodeStatus::Pending;
    node.timestamp = ultra::types::Timestamp::now();
    node.durationMs = 0;
    return node;
  }
};

TEST_F(ExecutionNodeTest, DefaultConstruction) {
  ExecutionNode node;
  EXPECT_EQ(node.nodeId, "");
  EXPECT_EQ(node.branchId, "");
  EXPECT_EQ(node.action, "");
  EXPECT_EQ(node.status, NodeStatus::Pending);
  EXPECT_EQ(node.durationMs, 0);
}

TEST_F(ExecutionNodeTest, NodeIdAssignment) {
  ExecutionNode node = createDefaultNode();
  EXPECT_EQ(node.nodeId, "test_node");
}

TEST_F(ExecutionNodeTest, BranchIdAssignment) {
  ExecutionNode node = createDefaultNode();
  EXPECT_EQ(node.branchId, "test_branch");
}

TEST_F(ExecutionNodeTest, ActionAssignment) {
  ExecutionNode node = createDefaultNode();
  EXPECT_EQ(node.action, "test_action");
}

TEST_F(ExecutionNodeTest, StatusAssignment) {
  ExecutionNode node = createDefaultNode();
  node.status = NodeStatus::Running;
  EXPECT_EQ(node.status, NodeStatus::Running);
}

TEST_F(ExecutionNodeTest, PendingStatus) {
  ExecutionNode node;
  EXPECT_EQ(node.status, NodeStatus::Pending);
}

TEST_F(ExecutionNodeTest, RunningStatus) {
  ExecutionNode node = createDefaultNode();
  node.status = NodeStatus::Running;
  EXPECT_EQ(node.status, NodeStatus::Running);
}

TEST_F(ExecutionNodeTest, SuccessStatus) {
  ExecutionNode node = createDefaultNode();
  node.status = NodeStatus::Success;
  EXPECT_EQ(node.status, NodeStatus::Success);
}

TEST_F(ExecutionNodeTest, FailedStatus) {
  ExecutionNode node = createDefaultNode();
  node.status = NodeStatus::Failed;
  EXPECT_EQ(node.status, NodeStatus::Failed);
}

TEST_F(ExecutionNodeTest, DurationAssignment) {
  ExecutionNode node = createDefaultNode();
  node.durationMs = 5000;
  EXPECT_EQ(node.durationMs, 5000);
}

TEST_F(ExecutionNodeTest, InputPayload) {
  ExecutionNode node = createDefaultNode();
  node.input = nlohmann::json::object();
  node.input["key"] = "value";
  EXPECT_EQ(node.input["key"], "value");
}

TEST_F(ExecutionNodeTest, OutputPayload) {
  ExecutionNode node = createDefaultNode();
  node.output = nlohmann::json::object();
  node.output["result"] = 42;
  EXPECT_EQ(node.output["result"], 42);
}

TEST_F(ExecutionNodeTest, ComplexInputPayload) {
  ExecutionNode node = createDefaultNode();
  node.input = nlohmann::json::object();
  node.input["nested"]["key"] = "nested_value";
  node.input["array"] = nlohmann::json::array();
  node.input["array"].push_back("item1");
  
  EXPECT_EQ(node.input["nested"]["key"], "nested_value");
  EXPECT_EQ(node.input["array"].size(), 1);
}

TEST_F(ExecutionNodeTest, ComplexOutputPayload) {
  ExecutionNode node = createDefaultNode();
  node.output = nlohmann::json::object();
  node.output["metrics"]["success"] = true;
  node.output["metrics"]["score"] = 0.95;
  
  EXPECT_TRUE(node.output["metrics"]["success"]);
  EXPECT_NEAR(node.output["metrics"]["score"], 0.95, 0.001);
}

TEST_F(ExecutionNodeTest, TimestampAssignment) {
  ExecutionNode node = createDefaultNode();
  auto now = ultra::types::Timestamp::now();
  node.timestamp = now;
  EXPECT_EQ(node.timestamp.toISO8601(), now.toISO8601());
}

TEST_F(ExecutionNodeTest, MultipleNodeInstances) {
  ExecutionNode n1 = createDefaultNode();
  n1.nodeId = "node1";
  
  ExecutionNode n2 = createDefaultNode();
  n2.nodeId = "node2";
  
  EXPECT_NE(n1.nodeId, n2.nodeId);
}

TEST_F(ExecutionNodeTest, NodeMetadataStorage) {
  ExecutionNode node = createDefaultNode();
  node.action = "AnalyzeComplexity";
  node.status = NodeStatus::Running;
  node.durationMs = 1234;
  
  EXPECT_EQ(node.action, "AnalyzeComplexity");
  EXPECT_EQ(node.status, NodeStatus::Running);
  EXPECT_EQ(node.durationMs, 1234);
}

TEST_F(ExecutionNodeTest, StateTransitionPendingToRunning) {
  ExecutionNode node = createDefaultNode();
  EXPECT_EQ(node.status, NodeStatus::Pending);
  
  node.status = NodeStatus::Running;
  EXPECT_EQ(node.status, NodeStatus::Running);
}

TEST_F(ExecutionNodeTest, StateTransitionRunningToSuccess) {
  ExecutionNode node = createDefaultNode();
  node.status = NodeStatus::Running;
  node.durationMs = 500;
  
  node.status = NodeStatus::Success;
  EXPECT_EQ(node.status, NodeStatus::Success);
}

TEST_F(ExecutionNodeTest, StateTransitionRunningToFailed) {
  ExecutionNode node = createDefaultNode();
  node.status = NodeStatus::Running;
  
  node.status = NodeStatus::Failed;
  EXPECT_EQ(node.status, NodeStatus::Failed);
}

TEST_F(ExecutionNodeTest, DurationTracking) {
  ExecutionNode node = createDefaultNode();
  node.durationMs = 0;
  EXPECT_EQ(node.durationMs, 0);
  
  node.durationMs = 10000;
  EXPECT_EQ(node.durationMs, 10000);
}

TEST_F(ExecutionNodeTest, LargeDuration) {
  ExecutionNode node = createDefaultNode();
  node.durationMs = 3600000;
  EXPECT_EQ(node.durationMs, 3600000);
}

TEST_F(ExecutionNodeTest, EmptyNodeData) {
  ExecutionNode node;
  EXPECT_EQ(node.nodeId, "");
  EXPECT_EQ(node.branchId, "");
  EXPECT_EQ(node.action, "");
  EXPECT_EQ(node.durationMs, 0);
}

TEST_F(ExecutionNodeTest, SpecialCharactersInAction) {
  ExecutionNode node = createDefaultNode();
  node.action = "Analyze@#$Complexity!";
  EXPECT_EQ(node.action, "Analyze@#$Complexity!");
}

TEST_F(ExecutionNodeTest, LongNodeId) {
  ExecutionNode node = createDefaultNode();
  node.nodeId = std::string(1000, 'a');
  EXPECT_EQ(node.nodeId.length(), 1000);
}

TEST_F(ExecutionNodeTest, CopySemantics) {
  ExecutionNode n1 = createDefaultNode();
  n1.status = NodeStatus::Success;
  
  ExecutionNode n2 = n1;
  EXPECT_EQ(n2.nodeId, n1.nodeId);
  EXPECT_EQ(n2.status, n1.status);
}

TEST_F(ExecutionNodeTest, AssignmentSemantics) {
  ExecutionNode n1 = createDefaultNode();
  n1.status = NodeStatus::Running;
  
  ExecutionNode n2;
  n2 = n1;
  EXPECT_EQ(n2.status, NodeStatus::Running);
}

TEST_F(ExecutionNodeTest, StabilityAcrossRepeatedUpdates) {
  ExecutionNode node = createDefaultNode();
  
  for (int i = 0; i < 10; ++i) {
    node.status = NodeStatus::Running;
    node.durationMs = 100 * i;
  }
  
  EXPECT_EQ(node.status, NodeStatus::Running);
  EXPECT_EQ(node.durationMs, 900);
}

TEST_F(ExecutionNodeTest, PayloadWithArrays) {
  ExecutionNode node = createDefaultNode();
  node.output = nlohmann::json::array();
  node.output.push_back("result1");
  node.output.push_back("result2");
  node.output.push_back("result3");
  
  EXPECT_EQ(node.output.size(), 3);
}

TEST_F(ExecutionNodeTest, PayloadWithNumbers) {
  ExecutionNode node = createDefaultNode();
  node.output = nlohmann::json::object();
  node.output["int_value"] = 42;
  node.output["float_value"] = 3.14159;
  node.output["large_int"] = 9223372036854775807LL;
  
  EXPECT_EQ(node.output["int_value"], 42);
  EXPECT_NEAR(node.output["float_value"], 3.14159, 0.00001);
}

TEST_F(ExecutionNodeTest, PayloadWithBooleans) {
  ExecutionNode node = createDefaultNode();
  node.output = nlohmann::json::object();
  node.output["success"] = true;
  node.output["failed"] = false;
  
  EXPECT_TRUE(node.output["success"]);
  EXPECT_FALSE(node.output["failed"]);
}

TEST_F(ExecutionNodeTest, ClearPayloads) {
  ExecutionNode node = createDefaultNode();
  node.input = nlohmann::json::object();
  node.input["data"] = "value";
  node.output = nlohmann::json::object();
  node.output["result"] = "result_value";
  
  node.input = nlohmann::json::object();
  node.output = nlohmann::json::object();
  
  EXPECT_TRUE(node.input.empty());
  EXPECT_TRUE(node.output.empty());
}
