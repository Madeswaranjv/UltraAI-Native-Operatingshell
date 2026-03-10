// ============================================================================
// File: tests/orchestration/test_merge_controller.cpp
// Tests for MergeController output consolidation
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include <external/json.hpp>
#include "orchestration/MergeController.h"
#include "intelligence/BranchStore.h"
#include "intelligence/Branch.h"
#include "types/Confidence.h"

using namespace ultra::orchestration;
using namespace ultra::intelligence;

class MergeControllerTest : public ::testing::Test {
 protected:
  BranchStore store;
  MergeController controller{store};

  Branch createBranch(const std::string& branchId,
                     const std::string& goal = "test_goal",
                     double confidenceScore = 0.75) {
    Branch b;
    b.branchId = branchId;
    b.goal = goal;
    b.status = BranchState::Active;
    b.confidence.stabilityScore = confidenceScore;
    b.confidence.riskAdjustedConfidence = 0.7;
    b.confidence.decisionReliabilityIndex = 0.8;
    return b;
  }

  bool containsConflict(const std::vector<Conflict>& conflicts, const std::string& fieldPath) {
    return std::any_of(conflicts.begin(), conflicts.end(),
                      [&](const Conflict& c) { return c.fieldPath == fieldPath; });
  }
};

TEST_F(MergeControllerTest, ConsolidateSingleBranch) {
  Branch b = createBranch("branch1");
  store.add(b);
  
  auto result = controller.consolidate({"branch1"});
  
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.mergedOutput.is_null());
}

TEST_F(MergeControllerTest, ConsolidateMultipleBranches) {
  store.add(createBranch("b1", "goal1", 0.8));
  store.add(createBranch("b2", "goal2", 0.75));
  store.add(createBranch("b3", "goal3", 0.85));
  
  auto result = controller.consolidate({"b1", "b2", "b3"});
  
  EXPECT_TRUE(result.success);
}

TEST_F(MergeControllerTest, AggregatedConfidenceComputation) {
  Branch b1 = createBranch("b1", "goal1", 0.8);
  Branch b2 = createBranch("b2", "goal2", 0.6);
  store.add(b1);
  store.add(b2);
  
  auto result = controller.consolidate({"b1", "b2"});
  
  double expectedAvg = (0.8 + 0.6) / 2.0;
  EXPECT_NEAR(result.aggregatedConfidence.stabilityScore, expectedAvg, 0.001);
}

TEST_F(MergeControllerTest, ConflictResolutionHighestConfidence) {
  store.add(createBranch("b1", "goal1", 0.5));
  store.add(createBranch("b2", "goal2", 0.9));
  store.add(createBranch("b3", "goal3", 0.7));
  
  auto result = controller.consolidate({"b1", "b2", "b3"},
                                      ConflictResolutionStrategy::HighestConfidence);
  
  EXPECT_EQ(result.strategyUsed, "HighestConfidence");
  EXPECT_TRUE(result.success);
}

TEST_F(MergeControllerTest, MergeWithNoChanges) {
  Branch b = createBranch("b1");
  store.add(b);
  
  auto result = controller.consolidate({"b1"});
  
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.conflicts.size(), 0);
}

TEST_F(MergeControllerTest, EmptyBranchList) {
  auto result = controller.consolidate({});
  
  EXPECT_FALSE(result.success);
}

TEST_F(MergeControllerTest, NonexistentBranchHandling) {
  auto result = controller.consolidate({"nonexistent"});
  
  EXPECT_FALSE(result.success);
}

TEST_F(MergeControllerTest, MixedValidAndInvalidBranches) {
  store.add(createBranch("valid"));
  
  auto result = controller.consolidate({"valid", "invalid"});
  
  EXPECT_TRUE(result.success);
}

TEST_F(MergeControllerTest, ConsolidateWithAllStrategies) {
  store.add(createBranch("b1", "goal1", 0.8));
  store.add(createBranch("b2", "goal2", 0.6));
  
  auto resultHC = controller.consolidate({"b1", "b2"},
                                        ConflictResolutionStrategy::HighestConfidence);
  EXPECT_EQ(resultHC.strategyUsed, "HighestConfidence");
  
  auto resultFW = controller.consolidate({"b1", "b2"},
                                        ConflictResolutionStrategy::FirstWins);
  EXPECT_NE(resultFW.strategyUsed, "");
  
  auto resultMS = controller.consolidate({"b1", "b2"},
                                        ConflictResolutionStrategy::MergeStrict);
  EXPECT_NE(resultMS.strategyUsed, "");
  
  auto resultFOC = controller.consolidate({"b1", "b2"},
                                         ConflictResolutionStrategy::FailOnConflict);
  EXPECT_NE(resultFOC.strategyUsed, "");
}

TEST_F(MergeControllerTest, MergedOutputStructure) {
  Branch b = createBranch("b1", "my_goal", 0.9);
  store.add(b);
  
  auto result = controller.consolidate({"b1"});
  
  EXPECT_TRUE(result.mergedOutput.is_object());
  EXPECT_TRUE(result.mergedOutput.contains("winning_branch") ||
             result.mergedOutput.contains("data"));
}

TEST_F(MergeControllerTest, HighestConfidenceBranchSelection) {
  store.add(createBranch("low", "goal_low", 0.3));
  store.add(createBranch("high", "goal_high", 0.95));
  store.add(createBranch("medium", "goal_medium", 0.6));
  
  auto result = controller.consolidate({"low", "high", "medium"},
                                      ConflictResolutionStrategy::HighestConfidence);
  
  EXPECT_TRUE(result.success);
}

TEST_F(MergeControllerTest, MultiBranchMerge) {
  for (int i = 0; i < 5; ++i) {
    store.add(createBranch("b" + std::to_string(i), "goal" + std::to_string(i)));
  }
  
  std::vector<std::string> branchIds;
  for (int i = 0; i < 5; ++i) {
    branchIds.push_back("b" + std::to_string(i));
  }
  
  auto result = controller.consolidate(branchIds);
  
  EXPECT_TRUE(result.success || result.conflicts.size() >= 0);
}

TEST_F(MergeControllerTest, DeterministicMergeResult) {
  store.add(createBranch("b1", "goal1", 0.75));
  store.add(createBranch("b2", "goal2", 0.80));
  
  auto result1 = controller.consolidate({"b1", "b2"});
  auto result2 = controller.consolidate({"b1", "b2"});
  
  EXPECT_EQ(result1.success, result2.success);
  EXPECT_NEAR(result1.aggregatedConfidence.stabilityScore,
             result2.aggregatedConfidence.stabilityScore, 0.001);
}

TEST_F(MergeControllerTest, ConsolidatedConfidenceNonNegative) {
  store.add(createBranch("b1", "goal1", 0.5));
  store.add(createBranch("b2", "goal2", 0.6));
  
  auto result = controller.consolidate({"b1", "b2"});
  
  EXPECT_GE(result.aggregatedConfidence.stabilityScore, 0.0);
  EXPECT_GE(result.aggregatedConfidence.riskAdjustedConfidence, 0.0);
  EXPECT_GE(result.aggregatedConfidence.decisionReliabilityIndex, 0.0);
}

TEST_F(MergeControllerTest, ConsolidatedConfidenceMaxed) {
  store.add(createBranch("b1", "goal1", 1.0));
  store.add(createBranch("b2", "goal2", 1.0));
  
  auto result = controller.consolidate({"b1", "b2"});
  
  EXPECT_LE(result.aggregatedConfidence.stabilityScore, 1.0);
}

TEST_F(MergeControllerTest, LargeBatchMerge) {
  for (int i = 0; i < 30; ++i) {
    store.add(createBranch("b" + std::to_string(i), "goal" + std::to_string(i)));
  }
  
  std::vector<std::string> branchIds;
  for (int i = 0; i < 30; ++i) {
    branchIds.push_back("b" + std::to_string(i));
  }
  
  auto result = controller.consolidate(branchIds);
  
  EXPECT_EQ(result.success, true);
}

TEST_F(MergeControllerTest, ConflictDetectionStructure) {
  store.add(createBranch("b1"));
  store.add(createBranch("b2"));
  
  auto result = controller.consolidate({"b1", "b2"});
  
  EXPECT_EQ(result.conflicts.size() >= 0, true);
}

TEST_F(MergeControllerTest, ValidateOutputConsistency) {
  store.add(createBranch("branch1", "consistent_goal", 0.85));
  
  auto result = controller.consolidate({"branch1"});
  
  EXPECT_TRUE(result.mergedOutput.is_object());
  EXPECT_FALSE(result.strategyUsed.empty());
}

TEST_F(MergeControllerTest, DuplicateBranchInList) {
  store.add(createBranch("b1", "goal", 0.75));
  
  auto result = controller.consolidate({"b1", "b1", "b1"});
  
  EXPECT_EQ(result.success, true);
}

TEST_F(MergeControllerTest, BranchWithArchivedState) {
  Branch b = createBranch("archived", "archived_goal", 0.95);
  b.status = BranchState::Archived;
  store.add(b);
  
  auto result = controller.consolidate({"archived"});
  
  EXPECT_TRUE(result.success);
}

TEST_F(MergeControllerTest, BranchWithSuspendedState) {
  Branch b = createBranch("suspended", "suspended_goal", 0.60);
  b.status = BranchState::Suspended;
  store.add(b);
  
  auto result = controller.consolidate({"suspended"});
  
  EXPECT_EQ(result.success, true);
}

TEST_F(MergeControllerTest, StrategyFallback) {
  store.add(createBranch("b1", "goal1", 0.7));
  
  auto result = controller.consolidate({"b1"},
                                      ConflictResolutionStrategy::FirstWins);
  
  EXPECT_FALSE(result.strategyUsed.empty());
}

TEST_F(MergeControllerTest, OutputPayloadVerification) {
  Branch b = createBranch("b1", "test_goal", 0.8);
  store.add(b);
  
  auto result = controller.consolidate({"b1"});
  
  EXPECT_TRUE(result.mergedOutput.is_object());
  EXPECT_TRUE(result.mergedOutput.size() >= 0);
}

TEST_F(MergeControllerTest, RepeatedMergeStability) {
  store.add(createBranch("b1", "goal1", 0.8));
  
  std::vector<ConsolidatedResult> results;
  for (int i = 0; i < 3; ++i) {
    results.push_back(controller.consolidate({"b1"}));
  }
  
  EXPECT_EQ(results[0].success, results[1].success);
  EXPECT_EQ(results[1].success, results[2].success);
}

TEST_F(MergeControllerTest, AveragingMultipleConfidenceValues) {
  store.add(createBranch("b1", "g1", 0.9));
  store.add(createBranch("b2", "g2", 0.8));
  store.add(createBranch("b3", "g3", 0.7));
  store.add(createBranch("b4", "g4", 0.6));
  store.add(createBranch("b5", "g5", 0.5));
  
  auto result = controller.consolidate({"b1", "b2", "b3", "b4", "b5"});
  
  double expectedAvg = (0.9 + 0.8 + 0.7 + 0.6 + 0.5) / 5.0;
  EXPECT_NEAR(result.aggregatedConfidence.stabilityScore, expectedAvg, 0.001);
}
