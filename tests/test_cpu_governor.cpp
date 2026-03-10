#include <gtest/gtest.h>

#include "runtime/CPUGovernor.h"

#include <string>

namespace {

std::string workloadName(const char* base, const int suffix) {
  return std::string(base) + "." + std::to_string(suffix);
}

}  // namespace

TEST(CPUGovernor, ThreadRecommendationWithinBounds) {
  ultra::runtime::CPUGovernor& governor = ultra::runtime::CPUGovernor::instance();

  const std::string heavy = workloadName("scanner.full_scan", 1);
  const std::string light = workloadName("context_compression", 1);

  governor.registerWorkload(heavy);
  governor.recordExecutionTime(heavy, 160.0);
  governor.registerWorkload(light);
  governor.recordExecutionTime(light, 12.0);

  const std::size_t heavyThreads = governor.recommendedThreadCount(16U, heavy);
  const std::size_t lightThreads = governor.recommendedThreadCount(16U, light);

  EXPECT_GE(heavyThreads, 1U);
  EXPECT_LE(heavyThreads, 16U);
  EXPECT_GE(lightThreads, 1U);
  EXPECT_LE(lightThreads, 16U);
  EXPECT_LE(heavyThreads, lightThreads);
}

TEST(CPUGovernor, WorkloadRegistrationWorks) {
  ultra::runtime::CPUGovernor& governor = ultra::runtime::CPUGovernor::instance();
  const std::string workload = workloadName("impact_prediction", 2);

  governor.registerWorkload(workload);
  const ultra::runtime::GovernorStats stats = governor.stats();

  const auto it = stats.workloads.find(workload);
  ASSERT_NE(it, stats.workloads.end());
  EXPECT_GE(stats.activeWorkloads, 1U);
  EXPECT_EQ(it->second.activeCount, 1U);
  EXPECT_GE(it->second.registrationCount, 1U);

  governor.recordExecutionTime(workload, 48.0);
}

TEST(CPUGovernor, ExecutionTimeTrackingWorks) {
  ultra::runtime::CPUGovernor& governor = ultra::runtime::CPUGovernor::instance();
  const std::string workload = workloadName("context_compression", 3);

  governor.registerWorkload(workload);
  governor.recordExecutionTime(workload, 20.0);
  governor.registerWorkload(workload);
  governor.recordExecutionTime(workload, 60.0);

  const ultra::runtime::GovernorStats stats = governor.stats();
  const auto it = stats.workloads.find(workload);
  ASSERT_NE(it, stats.workloads.end());
  EXPECT_EQ(it->second.activeCount, 0U);
  EXPECT_EQ(it->second.sampleCount, 2U);
  EXPECT_GT(it->second.averageExecutionMs, 20.0);
  EXPECT_LT(it->second.averageExecutionMs, 60.0);
}

TEST(CPUGovernor, GovernorCalibrationStable) {
  ultra::runtime::CPUGovernor& governor = ultra::runtime::CPUGovernor::instance();
  const std::string workload = workloadName("scanner.incremental", 4);

  for (int index = 0; index < 4; ++index) {
    governor.registerWorkload(workload);
    governor.recordExecutionTime(workload, 18.0 + static_cast<double>(index));
  }

  const ultra::runtime::GovernorStats firstStats = governor.stats();
  const std::size_t firstRecommendation =
      governor.recommendedThreadCount(12U, workload);
  const ultra::runtime::GovernorStats secondStats = governor.stats();
  const std::size_t secondRecommendation =
      governor.recommendedThreadCount(12U, workload);

  EXPECT_EQ(firstRecommendation, secondRecommendation);
  EXPECT_LE(firstStats.minRecommendedThreadCount,
            firstStats.maxRecommendedThreadCount);
  EXPECT_LE(secondStats.minRecommendedThreadCount,
            secondStats.maxRecommendedThreadCount);
  EXPECT_LE(firstStats.minRecommendedThreadCount, firstRecommendation);
  EXPECT_GE(firstStats.maxRecommendedThreadCount, firstRecommendation);
  EXPECT_GE(secondStats.calibrationCount, firstStats.calibrationCount);
}
