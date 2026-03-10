// ============================================================================
// File: tests/branch/test_branch_lifecycle.cpp
// Deterministic BranchLifecycle tests
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "intelligence/BranchLifecycle.h"
#include "intelligence/BranchStore.h"
#include "intelligence/Branch.h"
#include "intelligence/BranchState.h"
#include "memory/SnapshotChain.h"
#include "memory/StateGraph.h"

using namespace ultra::intelligence;
using namespace ultra::memory;

class BranchLifecycleTest : public ::testing::Test {
 protected:
  BranchStore store;
  SnapshotChain chain;
  StateGraph graph;
  BranchLifecycle lifecycle{store, chain, graph};
};

TEST_F(BranchLifecycleTest, SpawnNewBranch) {
  Branch spawned = lifecycle.spawn("", "root_goal");

  EXPECT_FALSE(spawned.branchId.empty());
  EXPECT_EQ(spawned.goal, "root_goal");
  EXPECT_EQ(spawned.status, BranchState::Active);
  EXPECT_GT(spawned.creationSequence, 0);
  EXPECT_EQ(spawned.creationSequence, spawned.lastMutationSequence);
}

TEST_F(BranchLifecycleTest, SpawnChildBranch) {
  Branch parent = lifecycle.spawn("", "parent_goal");
  Branch child = lifecycle.spawn(parent.branchId, "child_goal");

  EXPECT_EQ(child.parentId, parent.branchId);
  EXPECT_EQ(child.goal, "child_goal");
}

TEST_F(BranchLifecycleTest, SuspendActiveBranch) {
  Branch branch = lifecycle.spawn("", "goal");

  uint64_t before = branch.lastMutationSequence;

  EXPECT_TRUE(lifecycle.suspend(branch.branchId));

  Branch suspended = store.get(branch.branchId);
  EXPECT_EQ(suspended.status, BranchState::Suspended);
  EXPECT_GT(suspended.lastMutationSequence, before);
}

TEST_F(BranchLifecycleTest, ResumeSuspendedBranch) {
  Branch branch = lifecycle.spawn("", "goal");
  lifecycle.suspend(branch.branchId);

  uint64_t before = store.get(branch.branchId).lastMutationSequence;

  EXPECT_TRUE(lifecycle.resume(branch.branchId));

  Branch resumed = store.get(branch.branchId);
  EXPECT_EQ(resumed.status, BranchState::Active);
  EXPECT_GT(resumed.lastMutationSequence, before);
}

TEST_F(BranchLifecycleTest, MergeTwoBranches) {
  Branch source = lifecycle.spawn("", "source");
  Branch target = lifecycle.spawn("", "target");

  EXPECT_TRUE(lifecycle.merge(source.branchId, target.branchId));

  Branch merged = store.get(source.branchId);
  EXPECT_EQ(merged.status, BranchState::Merged);
}

TEST_F(BranchLifecycleTest, ArchiveBranch) {
  Branch branch = lifecycle.spawn("", "goal");

  EXPECT_TRUE(lifecycle.archive(branch.branchId));

  Branch archived = store.get(branch.branchId);
  EXPECT_EQ(archived.status, BranchState::Archived);
}

TEST_F(BranchLifecycleTest, RollbackBranch) {
  Branch branch = lifecycle.spawn("", "goal");

  EXPECT_TRUE(lifecycle.rollback(branch.branchId));

  Branch rolled = store.get(branch.branchId);
  EXPECT_EQ(rolled.status, BranchState::RolledBack);
}

TEST_F(BranchLifecycleTest, ParentChildLinkageAfterSpawn) {
  Branch parent = lifecycle.spawn("", "parent");
  Branch child = lifecycle.spawn(parent.branchId, "child");

  Branch parentUpdated = store.get(parent.branchId);

  EXPECT_TRUE(std::find(parentUpdated.subBranches.begin(),
                        parentUpdated.subBranches.end(),
                        child.branchId) != parentUpdated.subBranches.end());
}

TEST_F(BranchLifecycleTest, SnapshotAssignedOnSpawn) {
  Branch branch = lifecycle.spawn("", "goal");
  EXPECT_FALSE(branch.memorySnapshotId.empty());
}

TEST_F(BranchLifecycleTest, DeterministicSequenceProgression) {
  Branch b1 = lifecycle.spawn("", "g1");
  Branch b2 = lifecycle.spawn("", "g2");

  EXPECT_LT(b1.creationSequence, b2.creationSequence);
}

TEST_F(BranchLifecycleTest, TerminalStatesAreTerminal) {
  Branch branch = lifecycle.spawn("", "goal");

  lifecycle.archive(branch.branchId);

  EXPECT_FALSE(lifecycle.suspend(branch.branchId));
  EXPECT_FALSE(lifecycle.resume(branch.branchId));
}

TEST_F(BranchLifecycleTest, ConsistentBranchCreationDifferentIds) {
  Branch b1 = lifecycle.spawn("", "goal");
  Branch b2 = lifecycle.spawn("", "goal");

  EXPECT_NE(b1.branchId, b2.branchId);
  EXPECT_EQ(b1.goal, b2.goal);
}

TEST_F(BranchLifecycleTest, LargeBranchTree) {
  Branch root = lifecycle.spawn("", "root");

  for (int i = 0; i < 10; ++i) {
    lifecycle.spawn(root.branchId, "child_" + std::to_string(i));
  }

  Branch updated = store.get(root.branchId);
  EXPECT_EQ(updated.subBranches.size(), 10);
}