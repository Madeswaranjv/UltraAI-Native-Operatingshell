// ============================================================================
// File: tests/branch/test_branch_store.cpp
// Deterministic BranchStore tests
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "intelligence/BranchStore.h"
#include "intelligence/Branch.h"
#include "intelligence/BranchState.h"

using namespace ultra::intelligence;

class BranchStoreTest : public ::testing::Test {
 protected:
  BranchStore store;

  Branch createBranch(const std::string& goal,
                      const std::string& parentId = "",
                      BranchState status = BranchState::Active) {
    Branch b;
    b.parentId = parentId;
    b.goal = goal;
    b.status = status;
    return b;
  }
};

TEST_F(BranchStoreTest, CreateBranchAssignsIdAndSequence) {
  Branch b = createBranch("goal1");
  Branch created = store.create(b);

  EXPECT_FALSE(created.branchId.empty());
  EXPECT_GT(created.creationSequence, 0);
  EXPECT_EQ(created.creationSequence, created.lastMutationSequence);
}

TEST_F(BranchStoreTest, UpdateIncrementsMutationSequence) {
  Branch b = store.create(createBranch("goal1"));
  uint64_t originalSeq = b.creationSequence;

  b.goal = "updated_goal";
  store.update(b);

  Branch updated = store.get(b.branchId);
  EXPECT_GT(updated.lastMutationSequence, originalSeq);
}

TEST_F(BranchStoreTest, GetNonexistentBranch) {
  Branch b = store.get("invalid");
  EXPECT_TRUE(b.branchId.empty());
}

TEST_F(BranchStoreTest, ListByState) {
  store.create(createBranch("a", "", BranchState::Active));
  store.create(createBranch("b", "", BranchState::Suspended));

  auto active = store.listByState(BranchState::Active);
  EXPECT_EQ(active.size(), 1);
}