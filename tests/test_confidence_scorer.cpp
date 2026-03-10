// ============================================================================
// File: tests/orchestration/test_confidence_scorer.cpp
// Tests for ConfidenceScorer confidence computation
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "orchestration/ConfidenceScorer.h"
#include "intelligence/BranchStore.h"
#include "intelligence/Branch.h"
#include "types/Confidence.h"

using namespace ultra::orchestration;
using namespace ultra::intelligence;
using namespace ultra::types;

class ConfidenceScorerTest : public ::testing::Test {
 protected:
  BranchStore store;
  ConfidenceScorer scorer{store};

  Branch createBranch(const std::string& branchId,
                     BranchState status = BranchState::Active,
                     const std::string& memoryId = "mem1") {
    Branch b;
    b.branchId = branchId;
    b.status = status;
    b.memorySnapshotId = memoryId;
    b.goal = "test_goal";
    return b;
  }
};

TEST_F(ConfidenceScorerTest, ZeroConfidenceScenario) {
  Confidence conf = scorer.score("nonexistent");
  
  EXPECT_GE(conf.stabilityScore, 0.0);
  EXPECT_GE(conf.riskAdjustedConfidence, 0.0);
  EXPECT_GE(conf.decisionReliabilityIndex, 0.0);
}

TEST_F(ConfidenceScorerTest, ActiveBranchScoring) {
  Branch b = createBranch("active_branch", BranchState::Active);
  store.add(b);
  
  auto conf = scorer.score("active_branch");
  
  EXPECT_GT(conf.stabilityScore, 0.0);
  EXPECT_GT(conf.riskAdjustedConfidence, 0.0);
  EXPECT_GT(conf.decisionReliabilityIndex, 0.0);
}

TEST_F(ConfidenceScorerTest, ArchivedBranchScoring) {
  Branch b = createBranch("archived_branch", BranchState::Archived);
  store.add(b);
  
  auto conf = scorer.score("archived_branch");
  
  EXPECT_NEAR(conf.stabilityScore, 1.0, 0.001);
  EXPECT_NEAR(conf.riskAdjustedConfidence, 0.95, 0.001);
  EXPECT_NEAR(conf.decisionReliabilityIndex, 0.98, 0.001);
}

TEST_F(ConfidenceScorerTest, SuspendedBranchScoring) {
  Branch b = createBranch("suspended_branch", BranchState::Suspended);
  store.add(b);
  
  auto conf = scorer.score("suspended_branch");
  
  EXPECT_LE(conf.stabilityScore, 0.5);
  EXPECT_LE(conf.riskAdjustedConfidence, 0.5);
}

TEST_F(ConfidenceScorerTest, RolledBackBranchScoring) {
  Branch b = createBranch("rolledback_branch", BranchState::RolledBack);
  store.add(b);
  
  auto conf = scorer.score("rolledback_branch");
  
  EXPECT_LE(conf.stabilityScore, 0.5);
}

TEST_F(ConfidenceScorerTest, MissingMemoryContext) {
  Branch b = createBranch("no_mem", BranchState::Active, "");
  store.add(b);
  
  auto conf = scorer.score("no_mem");
  
  EXPECT_LT(conf.stabilityScore, 0.8);
}

TEST_F(ConfidenceScorerTest, WeightedConfidenceComputation) {
  Branch b = createBranch("weighted", BranchState::Archived);
  store.add(b);
  
  auto conf = scorer.score("weighted");
  
  EXPECT_GE(conf.stabilityScore, 0.0);
  EXPECT_LE(conf.stabilityScore, 1.0);
}

TEST_F(ConfidenceScorerTest, ScoreConsolidatedEmpty) {
  auto conf = scorer.scoreConsolidated({});
  
  EXPECT_EQ(conf.stabilityScore, 0.0);
  EXPECT_EQ(conf.riskAdjustedConfidence, 0.0);
  EXPECT_EQ(conf.decisionReliabilityIndex, 0.0);
}

TEST_F(ConfidenceScorerTest, ScoreConsolidatedSingleBranch) {
  store.add(createBranch("b1", BranchState::Active));
  
  auto conf = scorer.scoreConsolidated({"b1"});
  
  EXPECT_GT(conf.stabilityScore, 0.0);
}

TEST_F(ConfidenceScorerTest, ScoreConsolidatedMultipleBranches) {
  store.add(createBranch("b1", BranchState::Active));
  store.add(createBranch("b2", BranchState::Active));
  store.add(createBranch("b3", BranchState::Active));
  
  auto conf = scorer.scoreConsolidated({"b1", "b2", "b3"});
  
  EXPECT_GT(conf.stabilityScore, 0.0);
  EXPECT_GT(conf.riskAdjustedConfidence, 0.0);
}

TEST_F(ConfidenceScorerTest, AveragingConfidenceScores) {
  Branch b1 = createBranch("b1", BranchState::Archived);
  Branch b2 = createBranch("b2", BranchState::Active);
  store.add(b1);
  store.add(b2);
  
  auto conf = scorer.scoreConsolidated({"b1", "b2"});
  
  EXPECT_GT(conf.stabilityScore, 0.0);
  EXPECT_LE(conf.stabilityScore, 1.0);
}

TEST_F(ConfidenceScorerTest, ConsolidatedWithMissingBranches) {
  store.add(createBranch("exists", BranchState::Active));
  
  auto conf = scorer.scoreConsolidated({"exists", "missing"});
  
  EXPECT_GT(conf.stabilityScore, 0.0);
}

TEST_F(ConfidenceScorerTest, SingleHighConfidenceBranch) {
  store.add(createBranch("high", BranchState::Archived));
  
  auto conf = scorer.score("high");
  
  EXPECT_NEAR(conf.stabilityScore, 1.0, 0.001);
}

TEST_F(ConfidenceScorerTest, SingleLowConfidenceBranch) {
  Branch b = createBranch("low", BranchState::RolledBack, "");
  store.add(b);
  
  auto conf = scorer.score("low");
  
  EXPECT_LT(conf.stabilityScore, 0.5);
}

TEST_F(ConfidenceScorerTest, DeterministicScoring) {
  store.add(createBranch("det", BranchState::Active));
  
  auto conf1 = scorer.score("det");
  auto conf2 = scorer.score("det");
  
  EXPECT_NEAR(conf1.stabilityScore, conf2.stabilityScore, 0.001);
  EXPECT_NEAR(conf1.riskAdjustedConfidence, conf2.riskAdjustedConfidence, 0.001);
}

TEST_F(ConfidenceScorerTest, LargeBatchScoring) {
  for (int i = 0; i < 30; ++i) {
    store.add(createBranch("b" + std::to_string(i), BranchState::Active));
  }
  
  std::vector<std::string> branchIds;
  for (int i = 0; i < 30; ++i) {
    branchIds.push_back("b" + std::to_string(i));
  }
  
  auto conf = scorer.scoreConsolidated(branchIds);
  
  EXPECT_GT(conf.stabilityScore, 0.0);
  EXPECT_LE(conf.stabilityScore, 1.0);
}

TEST_F(ConfidenceScorerTest, NoNegativeConfidenceValues) {
  store.add(createBranch("neg_test", BranchState::Active));
  
  auto conf = scorer.score("neg_test");
  
  EXPECT_GE(conf.stabilityScore, 0.0);
  EXPECT_GE(conf.riskAdjustedConfidence, 0.0);
  EXPECT_GE(conf.decisionReliabilityIndex, 0.0);
}

TEST_F(ConfidenceScorerTest, NoOverflowLargeInput) {
  for (int i = 0; i < 100; ++i) {
    store.add(createBranch("b" + std::to_string(i), BranchState::Active));
  }
  
  std::vector<std::string> branchIds;
  for (int i = 0; i < 100; ++i) {
    branchIds.push_back("b" + std::to_string(i));
  }
  
  auto conf = scorer.scoreConsolidated(branchIds);
  
  EXPECT_LE(conf.stabilityScore, 1.0);
  EXPECT_LE(conf.riskAdjustedConfidence, 1.0);
  EXPECT_LE(conf.decisionReliabilityIndex, 1.0);
}

TEST_F(ConfidenceScorerTest, SubBranchAggregation) {
  Branch parent = createBranch("parent", BranchState::Active);
  parent.subBranches = {"child1", "child2"};
  store.add(parent);
  
  store.add(createBranch("child1", BranchState::Active));
  store.add(createBranch("child2", BranchState::Active));
  
  auto conf = scorer.score("parent");
  
  EXPECT_GT(conf.stabilityScore, 0.0);
}

TEST_F(ConfidenceScorerTest, WeightingBaseAndSubBranches) {
  Branch parent = createBranch("parent", BranchState::Archived);
  parent.subBranches = {"child"};
  store.add(parent);
  
  Branch child = createBranch("child", BranchState::Active);
  store.add(child);
  
  auto conf = scorer.score("parent");
  
  EXPECT_GT(conf.stabilityScore, 0.0);
  EXPECT_LE(conf.stabilityScore, 1.0);
}

TEST_F(ConfidenceScorerTest, EmptySubBranchList) {
  Branch b = createBranch("empty_sub", BranchState::Active);
  b.subBranches = {};
  store.add(b);
  
  auto conf = scorer.score("empty_sub");
  
  EXPECT_GT(conf.stabilityScore, 0.0);
}

TEST_F(ConfidenceScorerTest, NonexistentBranchReturnsDefault) {
  auto conf = scorer.score("nonexistent");
  
  EXPECT_EQ(conf.stabilityScore, 0.0);
  EXPECT_EQ(conf.riskAdjustedConfidence, 0.0);
  EXPECT_EQ(conf.decisionReliabilityIndex, 0.0);
}

TEST_F(ConfidenceScorerTest, NonexistentSubBranches) {
  Branch parent = createBranch("parent", BranchState::Active);
  parent.subBranches = {"nonexistent_child"};
  store.add(parent);
  
  auto conf = scorer.score("parent");
  
  EXPECT_GE(conf.stabilityScore, 0.0);
}

TEST_F(ConfidenceScorerTest, AllStateTypes) {
  store.add(createBranch("active", BranchState::Active));
  store.add(createBranch("archived", BranchState::Archived));
  store.add(createBranch("suspended", BranchState::Suspended));
  store.add(createBranch("rolledback", BranchState::RolledBack));
  
  auto conf_active = scorer.score("active");
  auto conf_archived = scorer.score("archived");
  auto conf_suspended = scorer.score("suspended");
  auto conf_rolledback = scorer.score("rolledback");
  
  EXPECT_GT(conf_archived.stabilityScore, conf_active.stabilityScore);
}

TEST_F(ConfidenceScorerTest, MemoryContextPenalty) {
  Branch with_mem = createBranch("with_mem", BranchState::Active, "mem_id");
  Branch without_mem = createBranch("without_mem", BranchState::Active, "");
  
  store.add(with_mem);
  store.add(without_mem);
  
  auto conf_with = scorer.score("with_mem");
  auto conf_without = scorer.score("without_mem");
  
  EXPECT_GT(conf_with.stabilityScore, conf_without.stabilityScore);
}

TEST_F(ConfidenceScorerTest, RepeatedScoringStability) {
  store.add(createBranch("stable", BranchState::Active));
  
  std::vector<Confidence> results;
  for (int i = 0; i < 5; ++i) {
    results.push_back(scorer.score("stable"));
  }
  
  for (int i = 1; i < results.size(); ++i) {
    EXPECT_NEAR(results[i].stabilityScore, results[i-1].stabilityScore, 0.001);
  }
}

TEST_F(ConfidenceScorerTest, ConsolidatedAveraging) {
  store.add(createBranch("b1", BranchState::Archived));
  store.add(createBranch("b2", BranchState::Suspended));
  
  auto conf = scorer.scoreConsolidated({"b1", "b2"});
  
  EXPECT_GT(conf.stabilityScore, 0.0);
  EXPECT_LE(conf.stabilityScore, 1.0);
}

TEST_F(ConfidenceScorerTest, ThreeComponentNormalization) {
  store.add(createBranch("norm_test", BranchState::Archived));
  
  auto conf = scorer.score("norm_test");
  
  EXPECT_LE(conf.stabilityScore, 1.0);
  EXPECT_LE(conf.riskAdjustedConfidence, 1.0);
  EXPECT_LE(conf.decisionReliabilityIndex, 1.0);
}
