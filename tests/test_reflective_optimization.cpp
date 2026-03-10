#include <gtest/gtest.h>

#include "calibration/WeightManager.h"
#include "memory/CognitiveMemoryManager.h"
#include "runtime/GraphSnapshot.h"
#include "memory/StateGraph.h"

#include <filesystem>
#include <cstdint>
#include <memory>

namespace {

ultra::runtime::GraphSnapshot makeSnapshot(const std::uint64_t version) {
  ultra::runtime::GraphSnapshot snapshot;
  snapshot.graph = std::make_shared<ultra::memory::StateGraph>();
  snapshot.version = version;
  snapshot.branch = ultra::runtime::BranchId::nil();
  return snapshot;
}

ultra::memory::PerformanceSnapshot makeReflectiveSignal() {
  ultra::memory::PerformanceSnapshot snapshot;
  snapshot.avgTokenSavingsRatio = 0.10;
  snapshot.avgLatencyMs = 180.0;
  snapshot.errorRate = 0.12;
  snapshot.hotSliceHitRate = 0.18;
  snapshot.contextReuseRate = 0.22;
  snapshot.compressionRatio = 0.88;
  snapshot.overlayReuseRate = 0.30;
  snapshot.impactPredictionAccuracy = 0.40;
  return snapshot;
}

}  // namespace

TEST(ReflectiveOptimization, ReflectiveLoopAdjustsWeightsSafely) {
  const std::filesystem::path root =
      std::filesystem::current_path() / "tmp_reflective_loop_safe_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  ultra::memory::CognitiveMemoryManager manager(root);
  const ultra::runtime::GraphSnapshot snapshot = makeSnapshot(23U);
  manager.bindToSnapshot(&snapshot);

  const ultra::runtime::RelevanceProfile baseline =
      manager.resolvedRelevanceProfile();
  const ultra::memory::PerformanceSnapshot signal = makeReflectiveSignal();
  manager.applyStrategicAdjustments(&signal);
  const ultra::runtime::RelevanceProfile updated =
      manager.resolvedRelevanceProfile();

  const double updatedTotal = updated.recencyWeight + updated.centralityWeight +
                              updated.usageWeight + updated.impactWeight;
  EXPECT_NEAR(updatedTotal, 1.0, 1e-9);
  EXPECT_GE(updated.recencyWeight, 0.0);
  EXPECT_GE(updated.centralityWeight, 0.0);
  EXPECT_GE(updated.usageWeight, 0.0);
  EXPECT_GE(updated.impactWeight, 0.0);
  EXPECT_NE(updated.recencyWeight, baseline.recencyWeight);

  ultra::calibration::WeightManager weights(root / ".ultra");
  for (const auto& [name, value] : weights.getAllWeights()) {
    (void)name;
    EXPECT_GE(value, 0.1f);
    EXPECT_LE(value, 5.0f);
  }

  std::filesystem::remove_all(root, ec);
}

TEST(ReflectiveOptimization, ReflectiveLoopProducesStableWeights) {
  const ultra::memory::PerformanceSnapshot signal = makeReflectiveSignal();
  const ultra::runtime::GraphSnapshot snapshot = makeSnapshot(31U);

  const std::filesystem::path rootA =
      std::filesystem::current_path() / "tmp_reflective_loop_stable_a";
  const std::filesystem::path rootB =
      std::filesystem::current_path() / "tmp_reflective_loop_stable_b";
  std::error_code ec;
  std::filesystem::remove_all(rootA, ec);
  std::filesystem::remove_all(rootB, ec);

  ultra::memory::CognitiveMemoryManager firstManager(rootA);
  ultra::memory::CognitiveMemoryManager secondManager(rootB);
  firstManager.bindToSnapshot(&snapshot);
  secondManager.bindToSnapshot(&snapshot);

  firstManager.applyStrategicAdjustments(&signal);
  secondManager.applyStrategicAdjustments(&signal);

  ultra::calibration::WeightManager firstWeights(rootA / ".ultra");
  ultra::calibration::WeightManager secondWeights(rootB / ".ultra");

  const auto firstAll = firstWeights.getAllWeights();
  const auto secondAll = secondWeights.getAllWeights();
  ASSERT_EQ(firstAll.size(), secondAll.size());
  for (const auto& [name, value] : firstAll) {
    const auto it = secondAll.find(name);
    ASSERT_NE(it, secondAll.end());
    EXPECT_FLOAT_EQ(value, it->second);
  }

  EXPECT_EQ(firstManager.governanceState().compressionDepth,
            secondManager.governanceState().compressionDepth);
  EXPECT_DOUBLE_EQ(firstManager.governanceState().tokenBudgetScale,
                   secondManager.governanceState().tokenBudgetScale);
  EXPECT_DOUBLE_EQ(firstManager.governanceState().pruningThreshold,
                   secondManager.governanceState().pruningThreshold);

  std::filesystem::remove_all(rootA, ec);
  std::filesystem::remove_all(rootB, ec);
}
