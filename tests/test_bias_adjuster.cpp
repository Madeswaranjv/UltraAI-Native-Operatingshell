// ============================================================================
// File: tests/calibration/test_bias_adjuster.cpp
// Tests for BiasAdjuster pattern-based weight adjustments
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include "calibration/BiasAdjuster.h"
#include "calibration/WeightManager.h"

using namespace ultra::calibration;
namespace fs = std::filesystem;

class BiasAdjusterTest : public ::testing::Test {
 protected:
  fs::path testDir;
  WeightManager* manager;
  BiasAdjuster* adjuster;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "bias_adjuster";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
    manager = new WeightManager(testDir);
    adjuster = new BiasAdjuster(*manager);
  }

  void TearDown() override {
    delete adjuster;
    delete manager;
    fs::remove_all(testDir);
  }
};

TEST_F(BiasAdjusterTest, ApplyDiffPatternBias) {
  manager->setWeight("risk_score_weight", 1.0f);
  
  adjuster->applyPatternBias("diff");
  
  float adjusted = manager->getWeight("risk_score_weight");
  EXPECT_GT(adjusted, 1.0f);
}

TEST_F(BiasAdjusterTest, ApplyAnalyzePatternBias) {
  manager->setWeight("risk_score_weight", 1.0f);
  
  adjuster->applyPatternBias("analyze");
  
  float adjusted = manager->getWeight("risk_score_weight");
  EXPECT_GT(adjusted, 1.0f);
}

TEST_F(BiasAdjusterTest, ApplyBuildPatternBias) {
  manager->setWeight("cache_retention_weight", 1.0f);
  
  adjuster->applyPatternBias("build");
  
  float adjusted = manager->getWeight("cache_retention_weight");
  EXPECT_GT(adjusted, 1.0f);
}

TEST_F(BiasAdjusterTest, ApplyBuildIncrementalPatternBias) {
  manager->setWeight("cache_retention_weight", 1.0f);
  
  adjuster->applyPatternBias("build-incremental");
  
  float adjusted = manager->getWeight("cache_retention_weight");
  EXPECT_GT(adjusted, 1.0f);
}

TEST_F(BiasAdjusterTest, UnknownPatternNoEffect) {
  manager->setWeight("weight", 2.0f);
  
  adjuster->applyPatternBias("unknown_pattern");
  
  float result = manager->getWeight("weight");
  EXPECT_FLOAT_EQ(result, 2.0f);
}

TEST_F(BiasAdjusterTest, PositiveBiasEffect) {
  manager->setWeight("test_weight", 1.0f);
  
  adjuster->applyPatternBias("diff");
  
  float biased = manager->getWeight("risk_score_weight");
  EXPECT_NEAR(biased, 1.0f * 1.05f, 0.001f);
}

TEST_F(BiasAdjusterTest, MultipleBiasApplications) {
  manager->setWeight("risk_score_weight", 1.0f);
  
  adjuster->applyPatternBias("diff");
  adjuster->applyPatternBias("diff");
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight");
  float expected = 1.0f * 1.05f * 1.05f * 1.05f;
  EXPECT_NEAR(result, expected, 0.001f);
}

TEST_F(BiasAdjusterTest, DifferentPatternsAffectDifferentWeights) {
  manager->setWeight("risk_score_weight", 1.0f);
  manager->setWeight("cache_retention_weight", 1.0f);
  
  adjuster->applyPatternBias("diff");
  float riskWeighted = manager->getWeight("risk_score_weight");
  
  manager->setWeight("cache_retention_weight", 1.0f);
  adjuster->applyPatternBias("build");
  float cacheWeighted = manager->getWeight("cache_retention_weight");
  
  EXPECT_NEAR(riskWeighted, 1.05f, 0.001f);
  EXPECT_NEAR(cacheWeighted, 1.05f, 0.001f);
}

TEST_F(BiasAdjusterTest, ZeroBiasOnZeroWeight) {
  manager->setWeight("risk_score_weight", 0.0f);
  
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_FLOAT_EQ(result, 0.0f);
}

TEST_F(BiasAdjusterTest, BiasOnLargeWeight) {
  manager->setWeight("risk_score_weight", 100.0f);
  
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_NEAR(result, 105.0f, 0.1f);
}

TEST_F(BiasAdjusterTest, BiasOnSmallWeight) {
  manager->setWeight("risk_score_weight", 0.1f);
  
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_NEAR(result, 0.105f, 0.001f);
}

TEST_F(BiasAdjusterTest, DeterministicBiasApplication) {
  manager->setWeight("risk_score_weight", 2.0f);
  adjuster->applyPatternBias("diff");
  float first = manager->getWeight("risk_score_weight");
  
  manager->setWeight("risk_score_weight", 2.0f);
  adjuster->applyPatternBias("diff");
  float second = manager->getWeight("risk_score_weight");
  
  EXPECT_FLOAT_EQ(first, second);
}

TEST_F(BiasAdjusterTest, BiasPersistence) {
  manager->setWeight("risk_score_weight", 1.0f);
  adjuster->applyPatternBias("diff");
  float biased = manager->getWeight("risk_score_weight");
  
  WeightManager manager2(testDir);
  float retrieved = manager2.getWeight("risk_score_weight");
  EXPECT_FLOAT_EQ(retrieved, biased);
}

TEST_F(BiasAdjusterTest, MultiplePatternSequence) {
  manager->setWeight("risk_score_weight", 1.0f);
  manager->setWeight("cache_retention_weight", 1.0f);
  
  adjuster->applyPatternBias("diff");
  adjuster->applyPatternBias("build");
  
  float riskResult = manager->getWeight("risk_score_weight");
  float cacheResult = manager->getWeight("cache_retention_weight");
  
  EXPECT_NEAR(riskResult, 1.05f, 0.001f);
  EXPECT_NEAR(cacheResult, 1.05f, 0.001f);
}

TEST_F(BiasAdjusterTest, BiasWithNegativeWeight) {
  manager->setWeight("risk_score_weight", -1.0f);
  
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_NEAR(result, -1.05f, 0.001f);
}

TEST_F(BiasAdjusterTest, CaseSensitivePattern) {
  manager->setWeight("risk_score_weight", 1.0f);
  
  adjuster->applyPatternBias("DIFF");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_FLOAT_EQ(result, 1.0f);
}

TEST_F(BiasAdjusterTest, StabilityAcrossRepeatedAdjustments) {
  manager->setWeight("risk_score_weight", 1.0f);
  
  float value1 = 1.0f;
  float value2 = 1.0f;
  
  for (int i = 0; i < 5; ++i) {
    adjuster->applyPatternBias("diff");
    if (i == 0) value1 = manager->getWeight("risk_score_weight");
  }
  
  manager->setWeight("risk_score_weight", 1.0f);
  for (int i = 0; i < 5; ++i) {
    adjuster->applyPatternBias("diff");
    if (i == 0) value2 = manager->getWeight("risk_score_weight");
  }
  
  EXPECT_FLOAT_EQ(value1, value2);
}

TEST_F(BiasAdjusterTest, DefaultWeightIfMissing) {
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight", 1.0f);
  EXPECT_GT(result, 1.0f);
}

TEST_F(BiasAdjusterTest, BiasMultipliedCorrectly) {
  manager->setWeight("risk_score_weight", 2.0f);
  
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_NEAR(result, 2.0f * 1.05f, 0.001f);
}

TEST_F(BiasAdjusterTest, NoBiasForNullPattern) {
  manager->setWeight("risk_score_weight", 1.5f);
  
  adjuster->applyPatternBias("");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_FLOAT_EQ(result, 1.5f);
}

TEST_F(BiasAdjusterTest, SequentialDiffBoosts) {
  manager->setWeight("risk_score_weight", 1.0f);
  
  for (int i = 0; i < 10; ++i) {
    adjuster->applyPatternBias("diff");
  }
  
  float result = manager->getWeight("risk_score_weight");
  float expected = 1.0f;
  for (int i = 0; i < 10; ++i) {
    expected *= 1.05f;
  }
  
  EXPECT_NEAR(result, expected, 0.01f);
}

TEST_F(BiasAdjusterTest, SequentialBuildBoosts) {
  manager->setWeight("cache_retention_weight", 1.0f);
  
  for (int i = 0; i < 10; ++i) {
    adjuster->applyPatternBias("build");
  }
  
  float result = manager->getWeight("cache_retention_weight");
  float expected = 1.0f;
  for (int i = 0; i < 10; ++i) {
    expected *= 1.05f;
  }
  
  EXPECT_NEAR(result, expected, 0.01f);
}

TEST_F(BiasAdjusterTest, BiasWithFractionalWeight) {
  manager->setWeight("risk_score_weight", 0.75f);
  
  adjuster->applyPatternBias("diff");
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_NEAR(result, 0.75f * 1.05f, 0.001f);
}

TEST_F(BiasAdjusterTest, NoNegativeOverflow) {
  manager->setWeight("risk_score_weight", -100.0f);
  
  for (int i = 0; i < 10; ++i) {
    adjuster->applyPatternBias("diff");
  }
  
  float result = manager->getWeight("risk_score_weight");
  EXPECT_TRUE(std::isfinite(result));
}
