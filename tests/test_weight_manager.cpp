// ============================================================================
// File: tests/calibration/test_weight_manager.cpp
// Tests for WeightManager weight storage and management
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "calibration/WeightManager.h"

using namespace ultra::calibration;
namespace fs = std::filesystem;

class WeightManagerTest : public ::testing::Test {
 protected:
  fs::path testDir;
  WeightManager* manager;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "weight_manager";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
    manager = new WeightManager(testDir);
  }

  void TearDown() override {
    delete manager;
    fs::remove_all(testDir);
  }

  bool weightsFileExists() const {
    return fs::exists(testDir / "calibration" / "weights.json");
  }
};

TEST_F(WeightManagerTest, DefaultWeightRetrieval) {
  float weight = manager->getWeight("nonexistent", 1.5f);
  EXPECT_FLOAT_EQ(weight, 1.5f);
}

TEST_F(WeightManagerTest, DefaultWeightDefault) {
  float weight = manager->getWeight("unknown");
  EXPECT_FLOAT_EQ(weight, 1.0f);
}

TEST_F(WeightManagerTest, SetAndGetWeight) {
  manager->setWeight("test_weight", 2.5f);
  float retrieved = manager->getWeight("test_weight");
  EXPECT_FLOAT_EQ(retrieved, 2.5f);
}

TEST_F(WeightManagerTest, UpdateExistingWeight) {
  manager->setWeight("my_weight", 1.0f);
  manager->setWeight("my_weight", 3.0f);
  float retrieved = manager->getWeight("my_weight");
  EXPECT_FLOAT_EQ(retrieved, 3.0f);
}

TEST_F(WeightManagerTest, DuplicateWeightUpdate) {
  manager->setWeight("dup", 1.0f);
  manager->setWeight("dup", 1.0f);
  manager->setWeight("dup", 2.0f);
  
  float retrieved = manager->getWeight("dup");
  EXPECT_FLOAT_EQ(retrieved, 2.0f);
}

TEST_F(WeightManagerTest, NegativeWeightHandling) {
  manager->setWeight("negative", -0.5f);
  float retrieved = manager->getWeight("negative");
  EXPECT_FLOAT_EQ(retrieved, -0.5f);
}

TEST_F(WeightManagerTest, LargeWeightValues) {
  manager->setWeight("large", 1000000.0f);
  float retrieved = manager->getWeight("large");
  EXPECT_FLOAT_EQ(retrieved, 1000000.0f);
}

TEST_F(WeightManagerTest, VerySmallWeightValues) {
  manager->setWeight("tiny", 0.0001f);
  float retrieved = manager->getWeight("tiny");
  EXPECT_NEAR(retrieved, 0.0001f, 0.00001f);
}

TEST_F(WeightManagerTest, ZeroWeight) {
  manager->setWeight("zero", 0.0f);
  float retrieved = manager->getWeight("zero");
  EXPECT_FLOAT_EQ(retrieved, 0.0f);
}

TEST_F(WeightManagerTest, MultipleWeightCategories) {
  manager->setWeight("category_a_weight1", 1.5f);
  manager->setWeight("category_a_weight2", 2.0f);
  manager->setWeight("category_b_weight1", 3.5f);
  manager->setWeight("category_b_weight2", 4.0f);
  
  EXPECT_FLOAT_EQ(manager->getWeight("category_a_weight1"), 1.5f);
  EXPECT_FLOAT_EQ(manager->getWeight("category_a_weight2"), 2.0f);
  EXPECT_FLOAT_EQ(manager->getWeight("category_b_weight1"), 3.5f);
  EXPECT_FLOAT_EQ(manager->getWeight("category_b_weight2"), 4.0f);
}

TEST_F(WeightManagerTest, GetAllWeights) {
  manager->setWeight("w1", 1.0f);
  manager->setWeight("w2", 2.0f);
  manager->setWeight("w3", 3.0f);
  
  auto all = manager->getAllWeights();
  EXPECT_EQ(all.size(), 3);
  EXPECT_FLOAT_EQ(all["w1"], 1.0f);
  EXPECT_FLOAT_EQ(all["w2"], 2.0f);
  EXPECT_FLOAT_EQ(all["w3"], 3.0f);
}

TEST_F(WeightManagerTest, PersistenceAfterSave) {
  manager->setWeight("persistent", 5.5f);
  EXPECT_TRUE(weightsFileExists());
  
  WeightManager manager2(testDir);
  float retrieved = manager2.getWeight("persistent");
  EXPECT_FLOAT_EQ(retrieved, 5.5f);
}

TEST_F(WeightManagerTest, SaveMultipleWeights) {
  manager->setWeight("weight_a", 1.1f);
  manager->setWeight("weight_b", 2.2f);
  manager->setWeight("weight_c", 3.3f);
  
  WeightManager manager2(testDir);
  EXPECT_FLOAT_EQ(manager2.getWeight("weight_a"), 1.1f);
  EXPECT_FLOAT_EQ(manager2.getWeight("weight_b"), 2.2f);
  EXPECT_FLOAT_EQ(manager2.getWeight("weight_c"), 3.3f);
}

TEST_F(WeightManagerTest, ManualSaveOperation) {
  manager->setWeight("test", 7.5f);
  EXPECT_TRUE(manager->save());
}

TEST_F(WeightManagerTest, Reset) {
  manager->setWeight("w1", 1.0f);
  manager->setWeight("w2", 2.0f);
  
  manager->reset();
  
  auto all = manager->getAllWeights();
  EXPECT_EQ(all.size(), 0);
}

TEST_F(WeightManagerTest, ResetClearsFile) {
  manager->setWeight("before_reset", 1.0f);
  EXPECT_TRUE(weightsFileExists());
  
  manager->reset();
  EXPECT_FALSE(weightsFileExists());
}

TEST_F(WeightManagerTest, ReloadFromDisk) {
  manager->setWeight("reload_test", 9.9f);
  
  bool loaded = manager->load();
  EXPECT_TRUE(loaded);
  
  float retrieved = manager->getWeight("reload_test");
  EXPECT_FLOAT_EQ(retrieved, 9.9f);
}

TEST_F(WeightManagerTest, LoadNonexistentFile) {
  manager->reset();
  
  bool loaded = manager->load();
  EXPECT_FALSE(loaded);
}

TEST_F(WeightManagerTest, StabilityAcrossMultipleUpdates) {
  manager->setWeight("stable", 1.0f);
  
  for (int i = 0; i < 10; ++i) {
    manager->setWeight("stable", 1.0f + i * 0.1f);
  }
  
  float final = manager->getWeight("stable");
  EXPECT_FLOAT_EQ(final, 1.9f);
}

TEST_F(WeightManagerTest, DeterministicResults) {
  manager->setWeight("det1", 2.5f);
  manager->setWeight("det2", 3.7f);
  
  float val1_first = manager->getWeight("det1");
  float val1_second = manager->getWeight("det1");
  float val2_first = manager->getWeight("det2");
  float val2_second = manager->getWeight("det2");
  
  EXPECT_FLOAT_EQ(val1_first, val1_second);
  EXPECT_FLOAT_EQ(val2_first, val2_second);
}

TEST_F(WeightManagerTest, SpecialCharacterWeightNames) {
  manager->setWeight("weight-with-dashes", 1.5f);
  manager->setWeight("weight_with_underscores", 2.5f);
  manager->setWeight("weight.with.dots", 3.5f);
  
  EXPECT_FLOAT_EQ(manager->getWeight("weight-with-dashes"), 1.5f);
  EXPECT_FLOAT_EQ(manager->getWeight("weight_with_underscores"), 2.5f);
  EXPECT_FLOAT_EQ(manager->getWeight("weight.with.dots"), 3.5f);
}

TEST_F(WeightManagerTest, LongWeightNames) {
  std::string longName(500, 'a');
  manager->setWeight(longName, 4.2f);
  float retrieved = manager->getWeight(longName);
  EXPECT_FLOAT_EQ(retrieved, 4.2f);
}

TEST_F(WeightManagerTest, EmptyStringWeightName) {
  manager->setWeight("", 5.5f);
  float retrieved = manager->getWeight("");
  EXPECT_FLOAT_EQ(retrieved, 5.5f);
}

TEST_F(WeightManagerTest, NegativeZeroWeight) {
  manager->setWeight("neg_zero", -0.0f);
  float retrieved = manager->getWeight("neg_zero");
  EXPECT_FLOAT_EQ(retrieved, 0.0f);
}

TEST_F(WeightManagerTest, InfinityWeight) {
  manager->setWeight("inf", std::numeric_limits<float>::infinity());
  float retrieved = manager->getWeight("inf");
  EXPECT_TRUE(std::isinf(retrieved));
}

TEST_F(WeightManagerTest, VeryLargeNumberOfWeights) {
  for (int i = 0; i < 100; ++i) {
    manager->setWeight("weight_" + std::to_string(i), i * 0.1f);
  }
  
  auto all = manager->getAllWeights();
  EXPECT_EQ(all.size(), 100);
}
