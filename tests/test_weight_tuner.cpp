// ============================================================================
// File: tests/calibration/test_weight_tuner.cpp
// Tests for WeightTuner dynamic weight adjustment
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include "calibration/WeightTuner.h"
#include "calibration/WeightManager.h"

using namespace ultra::calibration;
namespace fs = std::filesystem;

class WeightTunerTest : public ::testing::Test {
 protected:
  fs::path testDir;
  WeightManager* manager;
  WeightTuner* tuner;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "weight_tuner";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
    manager = new WeightManager(testDir);
    tuner = new WeightTuner(*manager);
  }

  void TearDown() override {
    delete tuner;
    delete manager;
    fs::remove_all(testDir);
  }

  const float learningRate = 0.05f;
  const float minWeight = 0.1f;
  const float maxWeight = 5.0f;
};

TEST_F(WeightTunerTest, IncreaseWeightOnPositiveFeedback) {
  manager->setWeight("test", 1.0f);
  tuner->tune("test", 1.0f);
  
  float newVal = manager->getWeight("test");
  EXPECT_GT(newVal, 1.0f);
}

TEST_F(WeightTunerTest, DecreaseWeightOnNegativeFeedback) {
  manager->setWeight("test", 1.0f);
  tuner->tune("test", -1.0f);
  
  float newVal = manager->getWeight("test");
  EXPECT_LT(newVal, 1.0f);
}

TEST_F(WeightTunerTest, NoChangeOnZeroFeedback) {
  manager->setWeight("test", 1.0f);
  tuner->tune("test", 0.0f);
  
  float newVal = manager->getWeight("test");
  EXPECT_FLOAT_EQ(newVal, 1.0f);
}

TEST_F(WeightTunerTest, MultiplePositiveIterations) {
  manager->setWeight("iterative", 1.0f);
  float prev = 1.0f;
  
  for (int i = 0; i < 5; ++i) {
    tuner->tune("iterative", 1.0f);
    float curr = manager->getWeight("iterative");
    EXPECT_GT(curr, prev);
    prev = curr;
  }
}

TEST_F(WeightTunerTest, MultipleNegativeIterations) {
  manager->setWeight("iterative", 1.0f);
  float prev = 1.0f;
  
  for (int i = 0; i < 5; ++i) {
    tuner->tune("iterative", -1.0f);
    float curr = manager->getWeight("iterative");
    EXPECT_LT(curr, prev);
    prev = curr;
  }
}

TEST_F(WeightTunerTest, MinimumWeightBoundary) {
  manager->setWeight("boundary", 0.15f);
  
  for (int i = 0; i < 100; ++i) {
    tuner->tune("boundary", -1.0f);
  }
  
  float final = manager->getWeight("boundary");
  EXPECT_GE(final, minWeight);
}

TEST_F(WeightTunerTest, MaximumWeightBoundary) {
  manager->setWeight("boundary", 4.9f);
  
  for (int i = 0; i < 100; ++i) {
    tuner->tune("boundary", 1.0f);
  }
  
  float final = manager->getWeight("boundary");
  EXPECT_LE(final, maxWeight);
}

TEST_F(WeightTunerTest, FeedbackClampingAbove1) {
  manager->setWeight("clamp", 1.0f);
  tuner->tune("clamp", 2.0f);
  
  float newVal = manager->getWeight("clamp");
  EXPECT_GT(newVal, 1.0f);
  EXPECT_LE(newVal, 1.05f);
}

TEST_F(WeightTunerTest, FeedbackClampingBelow1) {
  manager->setWeight("clamp", 1.0f);
  tuner->tune("clamp", -2.0f);
  
  float newVal = manager->getWeight("clamp");
  EXPECT_LT(newVal, 1.0f);
  EXPECT_GE(newVal, 0.95f);
}

TEST_F(WeightTunerTest, PositiveFeedbackMagnitude) {
  manager->setWeight("mag", 1.0f);
  tuner->tune("mag", 0.5f);
  
  float newVal = manager->getWeight("mag");
  float delta = newVal - 1.0f;
  float expectedDelta = 1.0f * learningRate * 0.5f;
  EXPECT_NEAR(delta, expectedDelta, 0.001f);
}

TEST_F(WeightTunerTest, NegativeFeedbackMagnitude) {
  manager->setWeight("mag", 1.0f);
  tuner->tune("mag", -0.5f);
  
  float newVal = manager->getWeight("mag");
  float delta = 1.0f - newVal;
  float expectedDelta = 1.0f * learningRate * 0.5f;
  EXPECT_NEAR(delta, expectedDelta, 0.001f);
}

TEST_F(WeightTunerTest, TuneNonexistentWeight) {
  tuner->tune("newweight", 1.0f);
  
  float retrieved = manager->getWeight("newweight");
  EXPECT_GT(retrieved, 1.0f);
}

TEST_F(WeightTunerTest, TuneMultipleWeights) {
  manager->setWeight("w1", 1.0f);
  manager->setWeight("w2", 1.0f);
  manager->setWeight("w3", 1.0f);
  
  tuner->tune("w1", 1.0f);
  tuner->tune("w2", -1.0f);
  tuner->tune("w3", 0.5f);
  
  float v1 = manager->getWeight("w1");
  float v2 = manager->getWeight("w2");
  float v3 = manager->getWeight("w3");
  
  EXPECT_GT(v1, 1.0f);
  EXPECT_LT(v2, 1.0f);
  EXPECT_GT(v3, 1.0f);
}

TEST_F(WeightTunerTest, StabilityOverRepeatedTuning) {
  manager->setWeight("stable", 1.0f);
  
  float prev = 1.0f;
  for (int i = 0; i < 3; ++i) {
    tuner->tune("stable", 0.1f);
    float curr = manager->getWeight("stable");
    EXPECT_NE(curr, prev);
    prev = curr;
  }
}

TEST_F(WeightTunerTest, AlternatingFeedback) {
  manager->setWeight("alternating", 1.0f);
  
  for (int i = 0; i < 10; ++i) {
    tuner->tune("alternating", 1.0f);
    tuner->tune("alternating", -1.0f);
  }
  
  float final = manager->getWeight("alternating");
  EXPECT_GE(final, minWeight);
  EXPECT_LE(final, maxWeight);
}

TEST_F(WeightTunerTest, LargeBatchTuning) {
  for (int i = 0; i < 30; ++i) {
    manager->setWeight("batch_" + std::to_string(i), 1.0f);
  }
  
  for (int i = 0; i < 30; ++i) {
    tuner->tune("batch_" + std::to_string(i), 0.5f);
  }
  
  for (int i = 0; i < 30; ++i) {
    float val = manager->getWeight("batch_" + std::to_string(i));
    EXPECT_GT(val, 1.0f);
    EXPECT_LE(val, maxWeight);
  }
}

TEST_F(WeightTunerTest, DeterministicTuning) {
  manager->setWeight("det1", 2.0f);
  manager->setWeight("det2", 2.0f);
  
  tuner->tune("det1", 0.7f);
  tuner->tune("det2", 0.7f);
  
  float val1 = manager->getWeight("det1");
  float val2 = manager->getWeight("det2");
  
  EXPECT_FLOAT_EQ(val1, val2);
}

TEST_F(WeightTunerTest, NoNegativeWeightAfterTuning) {
  manager->setWeight("poscheck", 0.2f);
  
  for (int i = 0; i < 50; ++i) {
    tuner->tune("poscheck", -1.0f);
  }
  
  float final = manager->getWeight("poscheck");
  EXPECT_GE(final, minWeight);
  EXPECT_GT(final, 0.0f);
}

TEST_F(WeightTunerTest, NoOverflowOnLargeInput) {
  manager->setWeight("overflow", 4.5f);
  
  for (int i = 0; i < 100; ++i) {
    tuner->tune("overflow", 1.0f);
  }
  
  float final = manager->getWeight("overflow");
  EXPECT_LE(final, maxWeight);
  EXPECT_TRUE(std::isfinite(final));
}

TEST_F(WeightTunerTest, ConvergenceBehavior) {
  manager->setWeight("convergence", 1.0f);
  
  float prev = 1.0f;
  int increments = 0;
  for (int i = 0; i < 20; ++i) {
    tuner->tune("convergence", 1.0f);
    float curr = manager->getWeight("convergence");
    if (curr > prev) increments++;
    prev = curr;
  }
  
  EXPECT_GT(increments, 0);
  EXPECT_LE(prev, maxWeight);
}

TEST_F(WeightTunerTest, SmallFeedbackSignal) {
  manager->setWeight("small", 1.0f);
  
  tuner->tune("small", 0.01f);
  float after = manager->getWeight("small");
  
  EXPECT_GT(after, 1.0f);
  EXPECT_LT(after - 1.0f, 0.01f);
}

TEST_F(WeightTunerTest, PersistenceAfterTuning) {
  manager->setWeight("persist", 1.0f);
  tuner->tune("persist", 1.0f);
  float tuned = manager->getWeight("persist");
  
  WeightManager manager2(testDir);
  float retrieved = manager2.getWeight("persist");
  EXPECT_FLOAT_EQ(retrieved, tuned);
}

TEST_F(WeightTunerTest, SequentialTuningConverges) {
  manager->setWeight("seq", 1.0f);
  
  std::vector<float> values;
  for (int i = 0; i < 5; ++i) {
    tuner->tune("seq", 1.0f);
    values.push_back(manager->getWeight("seq"));
  }
  
  for (int i = 1; i < values.size(); ++i) {
    EXPECT_GE(values[i], values[i-1]);
  }
}

TEST_F(WeightTunerTest, WeightScalesWithFeedbackStrength) {
  manager->setWeight("scale1", 1.0f);
  manager->setWeight("scale2", 1.0f);
  
  tuner->tune("scale1", 0.3f);
  tuner->tune("scale2", 0.9f);
  
  float val1 = manager->getWeight("scale1");
  float val2 = manager->getWeight("scale2");
  
  EXPECT_LT(val1, val2);
}
