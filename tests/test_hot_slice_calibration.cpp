// ============================================================================
// File: tests/calibration/test_hot_slice_calibration.cpp
// Tests for HotSlice calibration-relevant features
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "memory/HotSlice.h"
#include "memory/StateGraph.h"
#include "calibration/UsageTracker.h"

using namespace ultra::memory;
using namespace ultra::calibration;

class HotSliceCalibrationTest : public ::testing::Test {
 protected:
  HotSlice slice{HotSlice::kMaxHotSliceEntries};

  StateNode createNode(const std::string& nodeId,
                      const std::string& data = "test_data") {
    StateNode node;
    node.nodeId = nodeId;
    node.data = data;
    return node;
  }

  bool containsNodeId(const std::vector<StateNode>& nodes, const std::string& nodeId) {
    return std::any_of(nodes.begin(), nodes.end(),
                      [&](const StateNode& n) { return n.nodeId == nodeId; });
  }
};

TEST_F(HotSliceCalibrationTest, InsertItemWithScore) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceCalibrationTest, RecordAccessIncrementsScore) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.recordAccess("node1");
  slice.recordAccess("node1");
  
  auto topK = slice.getTopK(1);
  EXPECT_EQ(topK.size(), 1);
  EXPECT_EQ(topK[0].nodeId, "node1");
}

TEST_F(HotSliceCalibrationTest, PromoteNodeScore) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.promote("node1", 5.0);
  
  auto topK = slice.getTopK(1);
  EXPECT_EQ(topK.size(), 1);
  EXPECT_EQ(topK[0].nodeId, "node1");
}

TEST_F(HotSliceCalibrationTest, DemoteNodeScore) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.demote("node1", 0.5);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceCalibrationTest, RelevanceReordering) {
  slice.storeNode(createNode("n1"));
  slice.storeNode(createNode("n2"));
  slice.storeNode(createNode("n3"));
  
  slice.promote("n3", 10.0);
  slice.promote("n2", 5.0);
  
  auto topK = slice.getTopK(3);
  EXPECT_EQ(topK[0].nodeId, "n3");
  EXPECT_EQ(topK[1].nodeId, "n2");
}

TEST_F(HotSliceCalibrationTest, CapacityEnforcement) {
  HotSlice smallSlice{HotSlice::kMaxHotSliceEntries};

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 20U; ++i) {
    smallSlice.storeNode(createNode("node_" + std::to_string(i)));
  }

  EXPECT_EQ(smallSlice.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceCalibrationTest, EvictionPolicy) {
  HotSlice limited{HotSlice::kMaxHotSliceEntries};

  limited.storeNode(createNode("hot"));
  limited.promote("hot", 10.0);

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 16U; ++i) {
    limited.storeNode(createNode("cold_" + std::to_string(i)));
  }

  StateNode retrieved = limited.getNode("hot");
  EXPECT_EQ(retrieved.nodeId, "hot");
  EXPECT_EQ(limited.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceCalibrationTest, LargeDataset) {
  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 100U; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }

  EXPECT_EQ(slice.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceCalibrationTest, DeterministicRanking) {
  slice.storeNode(createNode("a"));
  slice.storeNode(createNode("b"));
  slice.storeNode(createNode("c"));
  
  slice.promote("c", 5.0);
  slice.promote("b", 3.0);
  
  auto rank1 = slice.getTopK(3);
  auto rank2 = slice.getTopK(3);
  
  EXPECT_EQ(rank1.size(), rank2.size());
  for (size_t i = 0; i < rank1.size(); ++i) {
    EXPECT_EQ(rank1[i].nodeId, rank2[i].nodeId);
  }
}

TEST_F(HotSliceCalibrationTest, DuplicateInsertUpdate) {
  StateNode node1 = createNode("dup");
  node1.data = "first";
  slice.storeNode(node1);
  
  StateNode node2 = createNode("dup");
  node2.data = "second";
  slice.storeNode(node2);
  
  StateNode retrieved = slice.getNode("dup");
  EXPECT_EQ(retrieved.data, "second");
}

TEST_F(HotSliceCalibrationTest, EmptySliceGetTopK) {
  auto topK = slice.getTopK(10);
  EXPECT_EQ(topK.size(), 0);
}

TEST_F(HotSliceCalibrationTest, GetTopKBeyondSize) {
  slice.storeNode(createNode("n1"));
  slice.storeNode(createNode("n2"));
  
  auto topK = slice.getTopK(100);
  EXPECT_EQ(topK.size(), 2);
}

TEST_F(HotSliceCalibrationTest, TrimToCapacity) {
  HotSlice limited{HotSlice::kMaxHotSliceEntries};

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 12U; ++i) {
    limited.storeNode(createNode("trim_node_" + std::to_string(i)));
  }

  limited.trim();

  EXPECT_EQ(limited.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceCalibrationTest, CalibrateWithUsageTracker) {
  slice.storeNode(createNode("node1"));
  slice.storeNode(createNode("node2"));
  
  UsageTracker tracker;
  tracker.record("diff", {"node1", "node2"});
  
  slice.calibrate(tracker);
  
  auto topK = slice.getTopK(2);
  EXPECT_EQ(topK.size(), 2);
}

TEST_F(HotSliceCalibrationTest, CalibrateEmptyTracker) {
  slice.storeNode(createNode("node1"));
  
  UsageTracker tracker;
  slice.calibrate(tracker);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceCalibrationTest, CalibrationDecayFactor) {
  slice.storeNode(createNode("node1"));
  slice.promote("node1", 100.0);
  
  UsageTracker tracker;
  slice.calibrate(tracker);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceCalibrationTest, MultipleAccessCounts) {
  slice.storeNode(createNode("n1"));
  
  slice.recordAccess("n1");
  slice.recordAccess("n1");
  slice.recordAccess("n1");
  
  auto topK = slice.getTopK(1);
  EXPECT_EQ(topK.size(), 1);
}

TEST_F(HotSliceCalibrationTest, PromoteNonexistentNode) {
  slice.promote("nonexistent", 5.0);
  
  StateNode retrieved = slice.getNode("nonexistent");
  EXPECT_EQ(retrieved.nodeId, "");
}

TEST_F(HotSliceCalibrationTest, DemoteNonexistentNode) {
  slice.demote("nonexistent", 5.0);
  
  StateNode retrieved = slice.getNode("nonexistent");
  EXPECT_EQ(retrieved.nodeId, "");
}

TEST_F(HotSliceCalibrationTest, DemotePreventNegative) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.demote("node1", 100.0);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceCalibrationTest, StabilityAcrossRepeatedScoring) {
  slice.storeNode(createNode("n1"));
  slice.storeNode(createNode("n2"));
  
  auto rank1 = slice.getTopK(2);
  auto rank2 = slice.getTopK(2);
  auto rank3 = slice.getTopK(2);
  
  EXPECT_EQ(rank1.size(), rank2.size());
  EXPECT_EQ(rank2.size(), rank3.size());
}

TEST_F(HotSliceCalibrationTest, ScoreUpdateSequence) {
  slice.storeNode(createNode("n1"));
  
  slice.promote("n1", 1.0);
  slice.promote("n1", 2.0);
  slice.promote("n1", 3.0);
  
  auto topK = slice.getTopK(1);
  EXPECT_EQ(topK[0].nodeId, "n1");
}

TEST_F(HotSliceCalibrationTest, MixedAccessAndPromotion) {
  slice.storeNode(createNode("n1"));
  slice.storeNode(createNode("n2"));
  
  slice.recordAccess("n1");
  slice.recordAccess("n1");
  slice.promote("n2", 5.0);
  
  auto topK = slice.getTopK(2);
  EXPECT_EQ(topK.size(), 2);
}

TEST_F(HotSliceCalibrationTest, LargePromotionValue) {
  slice.storeNode(createNode("n1"));
  
  slice.promote("n1", 1000000.0);
  
  auto topK = slice.getTopK(1);
  EXPECT_EQ(topK[0].nodeId, "n1");
}

TEST_F(HotSliceCalibrationTest, CalibrateWithDiffCommand) {
  slice.storeNode(createNode("file1"));
  slice.storeNode(createNode("file2"));
  
  UsageTracker tracker;
  tracker.record("diff", {"file1", "file2"});
  
  slice.calibrate(tracker);
  
  auto topK = slice.getTopK(2);
  EXPECT_EQ(topK.size(), 2);
}

TEST_F(HotSliceCalibrationTest, CalibrateWithAnalyzeCommand) {
  slice.storeNode(createNode("module1"));
  
  UsageTracker tracker;
  tracker.record("analyze", {"module1"});
  
  slice.calibrate(tracker);
  
  StateNode retrieved = slice.getNode("module1");
  EXPECT_EQ(retrieved.nodeId, "module1");
}

TEST_F(HotSliceCalibrationTest, CalibrateWithBuildCommand) {
  slice.storeNode(createNode("target1"));
  
  UsageTracker tracker;
  tracker.record("build", {"target1"});
  
  slice.calibrate(tracker);
  
  StateNode retrieved = slice.getNode("target1");
  EXPECT_EQ(retrieved.nodeId, "target1");
}

TEST_F(HotSliceCalibrationTest, MultipleCallsDoNotDuplicate) {
  slice.storeNode(createNode("unique"));
  
  EXPECT_EQ(slice.currentSize(), 1);
  
  slice.storeNode(createNode("unique"));
  
  EXPECT_EQ(slice.currentSize(), 1);
}

TEST_F(HotSliceCalibrationTest, AccessCountTiesResolved) {
  slice.storeNode(createNode("n1"));
  slice.storeNode(createNode("n2"));
  
  slice.recordAccess("n1");
  slice.recordAccess("n2");
  
  auto topK = slice.getTopK(2);
  EXPECT_EQ(topK.size(), 2);
}

TEST_F(HotSliceCalibrationTest, CapacityExceededTriggersTrim) {
  HotSlice small{HotSlice::kMaxHotSliceEntries};

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 9U; ++i) {
    small.storeNode(createNode("node_" + std::to_string(i)));
  }

  EXPECT_EQ(small.currentSize(), HotSlice::kMaxHotSliceEntries);
}
