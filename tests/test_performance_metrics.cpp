#include <gtest/gtest.h>

#include "metrics/PerformanceMetrics.h"

namespace {

class PerformanceMetricsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ultra::metrics::PerformanceMetrics::reset();
    ultra::metrics::PerformanceMetrics::setEnabled(true);
  }

  void TearDown() override {
    ultra::metrics::PerformanceMetrics::reset();
    ultra::metrics::PerformanceMetrics::setEnabled(false);
  }
};

}  // namespace

TEST_F(PerformanceMetricsTest, ReportUsesNestedRuntimeSchema) {
  ultra::metrics::SnapshotMetrics snapshot;
  snapshot.operation = "snapshot_create";
  snapshot.durationMicros = 120U;
  snapshot.nodeCount = 42U;
  ultra::metrics::PerformanceMetrics::recordSnapshotMetric(snapshot);

  ultra::metrics::ContextMetrics context;
  context.durationMicros = 80U;
  context.rawEstimatedTokens = 1000U;
  context.estimatedTokens = 250U;
  ultra::metrics::PerformanceMetrics::recordContextMetric(context);

  ultra::metrics::BranchMetrics branch;
  branch.operation = "branch_churn";
  branch.durationMicros = 200U;
  branch.evictionCount = 3U;
  ultra::metrics::PerformanceMetrics::recordBranchMetric(branch);

  ultra::metrics::PerformanceMetrics::recordTokenSavings(1000U, 250U);
  ultra::metrics::PerformanceMetrics::recordOverlayReuse(true);
  ultra::metrics::PerformanceMetrics::recordOverlayReuse(false);
  ultra::metrics::PerformanceMetrics::recordHotSliceLookup(3U, 4U);

  const nlohmann::ordered_json report =
      ultra::metrics::PerformanceMetrics::report();

  ASSERT_TRUE(report.value("enabled", false));
  ASSERT_TRUE(report.contains("snapshot"));
  ASSERT_TRUE(report.contains("context"));
  ASSERT_TRUE(report.contains("branch"));
  ASSERT_TRUE(report.contains("token"));

  const nlohmann::json snapshotReport =
      report.value("snapshot", nlohmann::json::object());
  EXPECT_DOUBLE_EQ(snapshotReport.value("avg_creation_time_micros", 0.0), 120.0);
  EXPECT_EQ(snapshotReport.value("max_creation_time_micros", 0ULL), 120ULL);
  ASSERT_TRUE(snapshotReport.contains("node_count_distribution"));
  ASSERT_EQ(snapshotReport["node_count_distribution"].size(), 1U);
  EXPECT_EQ(snapshotReport["node_count_distribution"][0].value("node_count", 0U),
            42U);

  const nlohmann::json contextReport =
      report.value("context", nlohmann::json::object());
  EXPECT_DOUBLE_EQ(contextReport.value("avg_compression_time_micros", 0.0), 80.0);
  EXPECT_DOUBLE_EQ(contextReport.value("avg_tokens_saved", 0.0), 750.0);
  EXPECT_DOUBLE_EQ(contextReport.value("compression_ratio", 0.0), 0.25);

  const nlohmann::json branchReport =
      report.value("branch", nlohmann::json::object());
  EXPECT_DOUBLE_EQ(branchReport.value("avg_churn_time_micros", 0.0), 200.0);
  EXPECT_EQ(branchReport.value("eviction_count", 0ULL), 3ULL);
  EXPECT_DOUBLE_EQ(branchReport.value("overlay_reuse_rate", 0.0), 0.5);

  const nlohmann::json tokenReport =
      report.value("token", nlohmann::json::object());
  EXPECT_EQ(tokenReport.value("total_tokens_saved", 0ULL), 750ULL);
  EXPECT_DOUBLE_EQ(tokenReport.value("avg_savings_percent", 0.0), 75.0);
  EXPECT_EQ(tokenReport.value("estimated_llm_calls_avoided", 0ULL), 0ULL);

  EXPECT_DOUBLE_EQ(report.value("avg_token_savings_ratio", 0.0), 0.75);
  EXPECT_NEAR(report.value("avg_latency_ms", 0.0), 0.133333, 1e-6);
  EXPECT_DOUBLE_EQ(report.value("error_rate", 1.0), 0.0);
  EXPECT_DOUBLE_EQ(report.value("hot_slice_hit_ratio", 0.0), 0.75);
}
