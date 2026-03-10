// ============================================================================
// File: tests/memory/test_hot_slice.cpp
// Tests for HotSlice adaptive cache
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "memory/HotSlice.h"
#include "memory/StateNode.h"

using namespace ultra::memory;

class HotSliceTest : public ::testing::Test {
 protected:
  HotSlice slice{HotSlice::kMaxHotSliceEntries};

  StateNode createNode(const std::string& nodeId,
                       NodeType nodeType = NodeType::Task) {
    StateNode node;
    node.nodeId = nodeId;
    node.nodeType = nodeType;
    node.data = nlohmann::json::parse("{}");
    node.version = 1;
    return node;
  }

  bool containsNode(const std::vector<StateNode>& nodes, const std::string& nodeId) {
    return std::any_of(nodes.begin(), nodes.end(),
                      [&](const StateNode& n) { return n.nodeId == nodeId; });
  }
};

TEST_F(HotSliceTest, InsertItem) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  EXPECT_EQ(slice.currentSize(), 1);
}

TEST_F(HotSliceTest, MultipleInserts) {
  for (int i = 0; i < 10; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  EXPECT_EQ(slice.currentSize(), 10);
}

TEST_F(HotSliceTest, AccessItem) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceTest, AccessNonexistentItem) {
  StateNode retrieved = slice.getNode("nonexistent");
  EXPECT_EQ(retrieved.nodeId, "");
}

TEST_F(HotSliceTest, RecordAccess) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.recordAccess("node1");
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceTest, RelevanceRanking) {
  for (int i = 0; i < 5; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  slice.recordAccess("node_0");
  slice.recordAccess("node_0");
  slice.recordAccess("node_1");
  
  auto topK = slice.getTopK(2);
  EXPECT_EQ(topK.size(), 2);
  EXPECT_EQ(topK[0].nodeId, "node_0");
}

TEST_F(HotSliceTest, GetTopK) {
  for (int i = 0; i < 10; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  auto topK = slice.getTopK(5);
  EXPECT_EQ(topK.size(), 5);
}

TEST_F(HotSliceTest, GetTopKMoreThanAvailable) {
  for (int i = 0; i < 3; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  auto topK = slice.getTopK(10);
  EXPECT_EQ(topK.size(), 3);
}

TEST_F(HotSliceTest, Promote) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.promote("node1", 5.0);
  
  auto topK = slice.getTopK(1);
  EXPECT_EQ(topK[0].nodeId, "node1");
}

TEST_F(HotSliceTest, PromoteNonexistentNode) {
  slice.promote("nonexistent", 1.0);
  EXPECT_EQ(slice.currentSize(), 0);
}

TEST_F(HotSliceTest, Demote) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.demote("node1", 0.5);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceTest, DemoteNonexistentNode) {
  slice.demote("nonexistent", 1.0);
  EXPECT_EQ(slice.currentSize(), 0);
}

TEST_F(HotSliceTest, DemotePreventNegativeScore) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.demote("node1", 100.0);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceTest, CapacityEnforcement) {
  HotSlice boundedSlice{HotSlice::kMaxHotSliceEntries};

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 16U; ++i) {
    boundedSlice.storeNode(createNode("node_" + std::to_string(i)));
  }

  EXPECT_EQ(boundedSlice.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceTest, EvictionBehavior) {
  HotSlice boundedSlice{HotSlice::kMaxHotSliceEntries};

  boundedSlice.storeNode(createNode("persistent_hot"));
  boundedSlice.promote("persistent_hot", 10.0);

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 8U; ++i) {
    boundedSlice.storeNode(createNode("cold_" + std::to_string(i)));
  }

  EXPECT_EQ(boundedSlice.currentSize(), HotSlice::kMaxHotSliceEntries);
  EXPECT_EQ(boundedSlice.getNode("persistent_hot").nodeId, "persistent_hot");
}

TEST_F(HotSliceTest, TrimFunction) {
  HotSlice mediumSlice{HotSlice::kMaxHotSliceEntries};

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries + 24U; ++i) {
    mediumSlice.storeNode(createNode("node_" + std::to_string(i)));
  }

  mediumSlice.trim();

  EXPECT_EQ(mediumSlice.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceTest, UpdateScore) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  slice.promote("node1", 2.0);
  slice.recordAccess("node1");
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceTest, LargeDataset50Items) {
  for (int i = 0; i < 50; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  EXPECT_EQ(slice.currentSize(), 50);
}

TEST_F(HotSliceTest, StabilityAcrossRepeatedUpdates) {
  for (int i = 0; i < 10; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  auto topK1 = slice.getTopK(5);
  
  slice.recordAccess("node_0");
  slice.recordAccess("node_1");
  
  auto topK2 = slice.getTopK(5);
  
  EXPECT_EQ(topK1.size(), topK2.size());
}

TEST_F(HotSliceTest, EmptySlice) {
  HotSlice empty{10};
  
  auto topK = empty.getTopK(5);
  EXPECT_TRUE(topK.empty());
  
  EXPECT_EQ(empty.currentSize(), 0);
}

TEST_F(HotSliceTest, DuplicateInsert) {
  StateNode node1 = createNode("node1");
  slice.storeNode(node1);
  
  StateNode node2 = createNode("node1");
  slice.storeNode(node2);
  
  EXPECT_EQ(slice.currentSize(), 1);
}

TEST_F(HotSliceTest, DuplicateInsertIncrementsAccess) {
  StateNode node1 = createNode("node1");
  slice.storeNode(node1);
  
  std::size_t initialSize = slice.currentSize();
  
  StateNode node2 = createNode("node1");
  slice.storeNode(node2);
  
  EXPECT_EQ(slice.currentSize(), initialSize);
}

TEST_F(HotSliceTest, GetTopKOrdering) {
  for (int i = 0; i < 5; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  for (int i = 0; i < 3; ++i) {
    slice.recordAccess("node_0");
  }
  slice.recordAccess("node_1");
  
  auto topK = slice.getTopK(3);
  
  EXPECT_EQ(topK.size(), 3);
  EXPECT_EQ(topK[0].nodeId, "node_0");
}

TEST_F(HotSliceTest, ZeroKQuery) {
  for (int i = 0; i < 5; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  auto topK = slice.getTopK(0);
  EXPECT_TRUE(topK.empty());
}

TEST_F(HotSliceTest, LargeK) {
  for (int i = 0; i < 5; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  auto topK = slice.getTopK(1000);
  EXPECT_EQ(topK.size(), 5);
}

TEST_F(HotSliceTest, AccessCountProgression) {
  StateNode node = createNode("node1");
  slice.storeNode(node);
  
  for (int i = 0; i < 10; ++i) {
    slice.recordAccess("node1");
  }
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
}

TEST_F(HotSliceTest, PromoteDemoteBalance) {
  StateNode node1 = createNode("node1");
  StateNode node2 = createNode("node2");
  
  slice.storeNode(node1);
  slice.storeNode(node2);
  
  slice.promote("node1", 3.0);
  slice.demote("node2", 2.0);
  
  auto topK = slice.getTopK(2);
  EXPECT_EQ(topK[0].nodeId, "node1");
}

TEST_F(HotSliceTest, NodeDataPreservation) {
  StateNode node = createNode("node1", NodeType::File);
  slice.storeNode(node);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeType, NodeType::File);
}

TEST_F(HotSliceTest, EvictionSelectsLowestRelevance) {
  HotSlice smallSlice{HotSlice::kMaxHotSliceEntries};

  StateNode node1 = createNode("node1");
  StateNode node2 = createNode("node2");
  StateNode node3 = createNode("node3");
  StateNode node4 = createNode("node4");

  node3.data["centrality"] = 0.1;
  node4.data["centrality"] = 0.1;

  smallSlice.storeNode(node1);
  smallSlice.storeNode(node2);
  smallSlice.storeNode(node3);

  smallSlice.promote("node1", 5.0);
  smallSlice.promote("node2", 3.0);

  smallSlice.storeNode(node4);
  smallSlice.evictLowestRelevance();

  EXPECT_EQ(smallSlice.currentSize(), 3);
  EXPECT_EQ(smallSlice.getNode("node4").nodeId, "");
  EXPECT_EQ(smallSlice.getNode("node3").nodeId, "node3");
}

TEST_F(HotSliceTest, RelevanceScoreTracking) {
  for (int i = 0; i < 5; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  slice.promote("node_0", 10.0);
  
  auto topK = slice.getTopK(1);
  EXPECT_EQ(topK[0].nodeId, "node_0");
}

TEST_F(HotSliceTest, ComplexAccessPattern) {
  for (int i = 0; i < 10; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  for (int i = 0; i < 5; ++i) {
    slice.recordAccess("node_0");
    slice.recordAccess("node_1");
  }
  
  slice.promote("node_2", 2.0);
  slice.demote("node_9", 1.0);
  
  auto topK = slice.getTopK(3);
  EXPECT_EQ(topK.size(), 3);
}

TEST_F(HotSliceTest, MaxCapacityLimit) {
  HotSlice limitedSlice{HotSlice::kMaxHotSliceEntries};

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries * 2U; ++i) {
    limitedSlice.storeNode(createNode("node_" + std::to_string(i)));
  }

  EXPECT_EQ(limitedSlice.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceTest, StoreExistingNodeUpdate) {
  StateNode node1 = createNode("node1");
  slice.storeNode(node1);
  
  StateNode node2 = createNode("node1");
  slice.storeNode(node2);
  
  StateNode retrieved = slice.getNode("node1");
  EXPECT_EQ(retrieved.nodeId, "node1");
  EXPECT_EQ(slice.currentSize(), 1);
}

TEST_F(HotSliceTest, TopKDeterminism) {
  for (int i = 0; i < 10; ++i) {
    slice.storeNode(createNode("node_" + std::to_string(i)));
  }
  
  for (int i = 0; i < 5; ++i) {
    slice.recordAccess("node_0");
    slice.recordAccess("node_1");
  }
  
  auto topK1 = slice.getTopK(3);
  auto topK2 = slice.getTopK(3);
  
  ASSERT_EQ(topK1.size(), topK2.size());
  for (size_t i = 0; i < topK1.size(); ++i) {
    EXPECT_EQ(topK1[i].nodeId, topK2[i].nodeId);
  }
}

TEST_F(HotSliceTest, HotSliceNeverExceedsCap) {
  HotSlice boundedSlice{HotSlice::kMaxHotSliceEntries};

  for (std::size_t i = 0; i < HotSlice::kMaxHotSliceEntries * 3U; ++i) {
    boundedSlice.storeNode(createNode("cap_node_" + std::to_string(i)));
    EXPECT_LE(boundedSlice.currentSize(), HotSlice::kMaxHotSliceEntries);
  }

  EXPECT_EQ(boundedSlice.currentSize(), HotSlice::kMaxHotSliceEntries);
}

TEST_F(HotSliceTest, DeterministicEvictionOrder) {
  HotSlice deterministicSlice{HotSlice::kMaxHotSliceEntries};

  StateNode nodeA = createNode("nodeA");
  StateNode nodeB = createNode("nodeB");
  StateNode nodeC = createNode("nodeC");

  nodeA.data["centrality"] = 0.2;
  nodeB.data["centrality"] = 0.2;
  nodeC.data["centrality"] = 0.9;

  deterministicSlice.storeNode(nodeA);
  deterministicSlice.storeNode(nodeB);
  deterministicSlice.storeNode(nodeC);
  deterministicSlice.promote("nodeC", 10.0);

  deterministicSlice.evictLowestRelevance();

  EXPECT_EQ(deterministicSlice.currentSize(), 2);
  EXPECT_EQ(deterministicSlice.getNode("nodeB").nodeId, "");
  EXPECT_EQ(deterministicSlice.getNode("nodeA").nodeId, "nodeA");
  EXPECT_EQ(deterministicSlice.getNode("nodeC").nodeId, "nodeC");
}

TEST_F(HotSliceTest, HonorsConfiguredCapacity) {
  HotSlice tinySlice{4U};
  for (std::size_t i = 0; i < 10U; ++i) {
    tinySlice.storeNode(createNode("tiny_" + std::to_string(i)));
  }
  EXPECT_EQ(tinySlice.currentSize(), 4U);
}
