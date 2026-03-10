// ============================================================================
// File: tests/branch/test_branch.cpp
// Deterministic Branch struct tests
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "intelligence/Branch.h"
#include "intelligence/BranchState.h"
#include "types/Confidence.h"

using namespace ultra::intelligence;
using namespace ultra::types;

class BranchTest : public ::testing::Test {
 protected:
  Branch createBranch(const std::string& goal = "test_goal",
                      BranchState status = BranchState::Active) {
    Branch b;
    b.goal = goal;
    b.status = status;
    b.confidence.stabilityScore = 1.0;
    return b;
  }

  bool containsSubBranch(const std::vector<std::string>& subs, const std::string& id) {
    return std::find(subs.begin(), subs.end(), id) != subs.end();
  }
};

TEST_F(BranchTest, DefaultBranchState) {
  Branch b;
  EXPECT_EQ(b.status, BranchState::Unknown);
}

TEST_F(BranchTest, GoalAssignment) {
  Branch b = createBranch();
  b.goal = "complex_reasoning_task";
  EXPECT_EQ(b.goal, "complex_reasoning_task");
}

TEST_F(BranchTest, ExecutionNodeIdTracking) {
  Branch b = createBranch();
  b.currentExecutionNodeId = "exec_node_42";
  EXPECT_EQ(b.currentExecutionNodeId, "exec_node_42");
}

TEST_F(BranchTest, SubBranchTracking) {
  Branch parent = createBranch();
  parent.subBranches.push_back("child1");
  parent.subBranches.push_back("child2");

  EXPECT_EQ(parent.subBranches.size(), 2);
  EXPECT_TRUE(containsSubBranch(parent.subBranches, "child1"));
  EXPECT_TRUE(containsSubBranch(parent.subBranches, "child2"));
}

TEST_F(BranchTest, MemorySnapshotIdAssignment) {
  Branch b = createBranch();
  b.memorySnapshotId = "snap_123";
  EXPECT_EQ(b.memorySnapshotId, "snap_123");
}

TEST_F(BranchTest, DependencyReferencesTracking) {
  Branch b = createBranch();
  b.dependencyReferences.push_back("dep1");
  b.dependencyReferences.push_back("dep2");

  EXPECT_EQ(b.dependencyReferences.size(), 2);
}

TEST_F(BranchTest, ConfidenceAssignment) {
  Branch b = createBranch();
  b.confidence.stabilityScore = 0.85;
  EXPECT_NEAR(b.confidence.stabilityScore, 0.85, 0.001);
}

TEST_F(BranchTest, SequenceFieldsDefaultZero) {
  Branch b;
  EXPECT_EQ(b.creationSequence, 0);
  EXPECT_EQ(b.lastMutationSequence, 0);
}