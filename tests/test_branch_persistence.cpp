// ============================================================================
// File: tests/branch/test_branch_persistence.cpp
// Deterministic persistence tests
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include "intelligence/BranchPersistence.h"
#include "intelligence/BranchStore.h"

using namespace ultra::intelligence;

class BranchPersistenceTest : public ::testing::Test {
 protected:
  std::filesystem::path testDir = std::filesystem::temp_directory_path() / "ultra_branch_tests";
  BranchStore store;
  BranchPersistence persistence{testDir};

  void SetUp() override {
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
  }

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

TEST_F(BranchPersistenceTest, RoundTripIntegrity) {
  Branch original = store.create(createBranch("complex_goal", "parent"));
  persistence.save(store);

  BranchStore loaded;
  persistence.load(loaded);

  Branch loadedBranch = loaded.get(original.branchId);

  EXPECT_EQ(loadedBranch.goal, original.goal);
  EXPECT_EQ(loadedBranch.parentId, original.parentId);
  EXPECT_EQ(loadedBranch.creationSequence, original.creationSequence);
}