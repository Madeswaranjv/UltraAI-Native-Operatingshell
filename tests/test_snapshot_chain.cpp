// ============================================================================
// File: tests/memory/test_snapshot_chain.cpp
// Tests for SnapshotChain linear snapshot management
// ============================================================================
//E:\Projects\Ultra\tests\test_snapshot_chain.cpp
#include <gtest/gtest.h>
#include <algorithm>
#include "memory/SnapshotChain.h"
#include "memory/StateSnapshot.h"

using namespace ultra::memory;

class SnapshotChainTest : public ::testing::Test {
 protected:
  SnapshotChain chain;

  StateSnapshot createSnapshot(const std::string& snapshotId,
                               std::size_t nodeCount = 1,
                               std::size_t edgeCount = 0) {
    StateSnapshot snap;
    snap.snapshotId = snapshotId;
    snap.nodeCount = nodeCount;
    snap.edgeCount = edgeCount;
    snap.graphHash = "hash_" + snapshotId;
    return snap;
  }

  bool containsSnapshot(const std::vector<StateSnapshot>& snaps,
                       const std::string& snapshotId) {
    return std::any_of(snaps.begin(), snaps.end(),
                      [&](const StateSnapshot& s) { return s.snapshotId == snapshotId; });
  }
};

TEST_F(SnapshotChainTest, CreateInitialSnapshot) {
  StateSnapshot snap = createSnapshot("S0");
  chain.append(snap);
  
  StateSnapshot current = chain.current();
  EXPECT_EQ(current.snapshotId, "S0");
}

TEST_F(SnapshotChainTest, CreateSubsequentSnapshots) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  
  StateSnapshot current = chain.current();
  EXPECT_EQ(current.snapshotId, "S2");
}

TEST_F(SnapshotChainTest, ValidateParentLinkage) {
  chain.append(createSnapshot("S0", 1, 0));
  chain.append(createSnapshot("S1", 2, 1));
  chain.append(createSnapshot("S2", 2, 2));
  
  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), 3);
  EXPECT_EQ(history[0].snapshotId, "S0");
  EXPECT_EQ(history[1].snapshotId, "S1");
  EXPECT_EQ(history[2].snapshotId, "S2");
}

TEST_F(SnapshotChainTest, ValidateImmutabilityOfPreviousSnapshots) {
  StateSnapshot snap0 = createSnapshot("S0", 1, 0);
  chain.append(snap0);
  
  StateSnapshot retrieved0 = chain.getSnapshot("S0");
  EXPECT_EQ(retrieved0.nodeCount, 1);
  EXPECT_EQ(retrieved0.edgeCount, 0);
  
  chain.append(createSnapshot("S1", 5, 10));
  
  StateSnapshot stillRetrieved0 = chain.getSnapshot("S0");
  EXPECT_EQ(stillRetrieved0.nodeCount, 1);
  EXPECT_EQ(stillRetrieved0.edgeCount, 0);
}

TEST_F(SnapshotChainTest, SnapshotRetrievalById) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  
  StateSnapshot snap1 = chain.getSnapshot("S1");
  EXPECT_EQ(snap1.snapshotId, "S1");
}

TEST_F(SnapshotChainTest, SnapshotRetrievalNonexistent) {
  chain.append(createSnapshot("S0"));
  
  StateSnapshot snap = chain.getSnapshot("nonexistent");
  EXPECT_EQ(snap.snapshotId, "");
}

TEST_F(SnapshotChainTest, SnapshotRollback) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  chain.append(createSnapshot("S3"));
  
  bool success = chain.rollback("S1");
  EXPECT_TRUE(success);
  
  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history[0].snapshotId, "S1");
}

TEST_F(SnapshotChainTest, RollbackToLatest) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  
  bool success = chain.rollback("S2");
  EXPECT_TRUE(success);
  
  auto history = chain.getHistory();
  EXPECT_EQ(history.size(), SnapshotChain::kMaxSnapshotsRetained);
}

TEST_F(SnapshotChainTest, RollbackToNonexistent) {
  chain.append(createSnapshot("S0"));
  
  bool success = chain.rollback("nonexistent");
  EXPECT_FALSE(success);
}

TEST_F(SnapshotChainTest, RollbackToEarliest) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  chain.append(createSnapshot("S3"));
  
  bool success = chain.rollback("S0");
  EXPECT_FALSE(success);
  
  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), SnapshotChain::kMaxSnapshotsRetained);
  EXPECT_EQ(history[0].snapshotId, "S1");
  EXPECT_EQ(history[1].snapshotId, "S2");
  EXPECT_EQ(history[2].snapshotId, "S3");
}

TEST_F(SnapshotChainTest, LongChain10Snapshots) {
  for (int i = 0; i < 10; ++i) {
    chain.append(createSnapshot("S" + std::to_string(i)));
  }
  
  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), SnapshotChain::kMaxSnapshotsRetained);
  EXPECT_EQ(history[0].snapshotId, "S7");
  EXPECT_EQ(history[1].snapshotId, "S8");
  EXPECT_EQ(history[2].snapshotId, "S9");
  
  StateSnapshot current = chain.current();
  EXPECT_EQ(current.snapshotId, "S9");
}

TEST_F(SnapshotChainTest, TemporalOrderingCorrectness) {
  for (int i = 0; i < 5; ++i) {
    chain.append(createSnapshot("S" + std::to_string(i), i, i));
  }
  
  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), SnapshotChain::kMaxSnapshotsRetained);
  for (size_t i = 0; i < history.size(); ++i) {
    EXPECT_EQ(history[i].snapshotId, "S" + std::to_string(i + 2U));
  }
}

TEST_F(SnapshotChainTest, StabilityAcrossRepeatedCreation) {
  for (int i = 0; i < 5; ++i) {
    chain.append(createSnapshot("S" + std::to_string(i)));
  }
  
  auto history1 = chain.getHistory();
  auto history2 = chain.getHistory();
  
  ASSERT_EQ(history1.size(), history2.size());
  for (size_t i = 0; i < history1.size(); ++i) {
    EXPECT_EQ(history1[i].snapshotId, history2[i].snapshotId);
  }
}

TEST_F(SnapshotChainTest, EmptyChainCurrent) {
  StateSnapshot current = chain.current();
  EXPECT_EQ(current.snapshotId, "");
}

TEST_F(SnapshotChainTest, EmptyChainHistory) {
  auto history = chain.getHistory();
  EXPECT_TRUE(history.empty());
}

TEST_F(SnapshotChainTest, EmptyChainSnapshot) {
  StateSnapshot snap = chain.getSnapshot("any");
  EXPECT_EQ(snap.snapshotId, "");
}

TEST_F(SnapshotChainTest, InvalidSnapshotId) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  
  StateSnapshot snap = chain.getSnapshot("S99");
  EXPECT_EQ(snap.snapshotId, "");
}

TEST_F(SnapshotChainTest, SnapshotMetadataValidation) {
  StateSnapshot snap = createSnapshot("S0", 42, 7);
  snap.graphHash = "abc123xyz";
  
  chain.append(snap);
  
  StateSnapshot retrieved = chain.getSnapshot("S0");
  EXPECT_EQ(retrieved.nodeCount, 42);
  EXPECT_EQ(retrieved.edgeCount, 7);
  EXPECT_EQ(retrieved.graphHash, "abc123xyz");
}

TEST_F(SnapshotChainTest, ClearChain) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  
  chain.clear();
  
  auto history = chain.getHistory();
  EXPECT_TRUE(history.empty());
  
  StateSnapshot current = chain.current();
  EXPECT_EQ(current.snapshotId, "");
}

TEST_F(SnapshotChainTest, AppendAfterClear) {
  chain.append(createSnapshot("S0"));
  chain.clear();
  chain.append(createSnapshot("S1"));
  
  auto history = chain.getHistory();
  EXPECT_EQ(history.size(), 1);
  EXPECT_EQ(history[0].snapshotId, "S1");
}

TEST_F(SnapshotChainTest, MultipleRollbacks) {
  for (int i = 0; i < 5; ++i) {
    chain.append(createSnapshot("S" + std::to_string(i)));
  }
  
  chain.rollback("S3");
  auto history1 = chain.getHistory();
  ASSERT_EQ(history1.size(), 2U);
  EXPECT_EQ(history1[0].snapshotId, "S2");
  EXPECT_EQ(history1[1].snapshotId, "S3");
  
  chain.rollback("S2");
  auto history2 = chain.getHistory();
  ASSERT_EQ(history2.size(), 1U);
  EXPECT_EQ(history2[0].snapshotId, "S2");
}

TEST_F(SnapshotChainTest, SnapshotNodeCountProgression) {
  chain.append(createSnapshot("S0", 1, 0));
  chain.append(createSnapshot("S1", 2, 1));
  chain.append(createSnapshot("S2", 3, 2));
  
  auto history = chain.getHistory();
  EXPECT_EQ(history[0].nodeCount, 1);
  EXPECT_EQ(history[1].nodeCount, 2);
  EXPECT_EQ(history[2].nodeCount, 3);
}

TEST_F(SnapshotChainTest, SnapshotEdgeCountProgression) {
  chain.append(createSnapshot("S0", 1, 0));
  chain.append(createSnapshot("S1", 2, 1));
  chain.append(createSnapshot("S2", 3, 3));
  
  auto history = chain.getHistory();
  EXPECT_EQ(history[0].edgeCount, 0);
  EXPECT_EQ(history[1].edgeCount, 1);
  EXPECT_EQ(history[2].edgeCount, 3);
}

TEST_F(SnapshotChainTest, LargeChain50Snapshots) {
  for (int i = 0; i < 50; ++i) {
    chain.append(createSnapshot("S" + std::to_string(i)));
  }
  
  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), SnapshotChain::kMaxSnapshotsRetained);
  EXPECT_EQ(history[0].snapshotId, "S47");
  EXPECT_EQ(history[1].snapshotId, "S48");
  EXPECT_EQ(history[2].snapshotId, "S49");
}

TEST_F(SnapshotChainTest, CurrentAfterRollback) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  
  chain.rollback("S1");
  
  StateSnapshot current = chain.current();
  EXPECT_EQ(current.snapshotId, "S1");
}

TEST_F(SnapshotChainTest, HistoryConsistency) {
  std::vector<std::string> snapshotIds = {"S0", "S1", "S2", "S3", "S4"};
  
  for (const auto& id : snapshotIds) {
    chain.append(createSnapshot(id));
  }
  
  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), SnapshotChain::kMaxSnapshotsRetained);
  for (size_t i = 0; i < history.size(); ++i) {
    EXPECT_EQ(history[i].snapshotId, snapshotIds[snapshotIds.size() - history.size() + i]);
  }
}

TEST_F(SnapshotChainTest, SnapshotHashConsistency) {
  StateSnapshot snap = createSnapshot("S0");
  snap.graphHash = "hash_value_123";
  
  chain.append(snap);
  
  StateSnapshot retrieved = chain.getSnapshot("S0");
  EXPECT_EQ(retrieved.graphHash, "hash_value_123");
}

TEST_F(SnapshotChainTest, RollbackPreservesEarlierSnapshots) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  chain.append(createSnapshot("S3"));
  
  chain.rollback("S2");
  
  StateSnapshot snap0 = chain.getSnapshot("S0");
  StateSnapshot snap1 = chain.getSnapshot("S1");
  StateSnapshot snap2 = chain.getSnapshot("S2");
  
  EXPECT_EQ(snap0.snapshotId, "");
  EXPECT_EQ(snap1.snapshotId, "S1");
  EXPECT_EQ(snap2.snapshotId, "S2");
}

TEST_F(SnapshotChainTest, SnapshotRetentionCapEnforced) {
  for (std::size_t i = 0; i < SnapshotChain::kMaxSnapshotsRetained + 4U; ++i) {
    chain.append(createSnapshot("S" + std::to_string(i)));
  }

  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), SnapshotChain::kMaxSnapshotsRetained);
  EXPECT_EQ(history[0].snapshotId, "S4");
  EXPECT_EQ(history[1].snapshotId, "S5");
  EXPECT_EQ(history[2].snapshotId, "S6");
}

TEST_F(SnapshotChainTest, RollbackWithinRetentionOnly) {
  for (std::size_t i = 0; i < SnapshotChain::kMaxSnapshotsRetained + 2U; ++i) {
    chain.append(createSnapshot("S" + std::to_string(i)));
  }

  EXPECT_FALSE(chain.rollback("S1"));
  EXPECT_TRUE(chain.rollback("S3"));

  auto history = chain.getHistory();
  ASSERT_EQ(history.size(), 2U);
  EXPECT_EQ(history[0].snapshotId, "S2");
  EXPECT_EQ(history[1].snapshotId, "S3");
}

TEST_F(SnapshotChainTest, RollbackRemovesLaterSnapshots) {
  chain.append(createSnapshot("S0"));
  chain.append(createSnapshot("S1"));
  chain.append(createSnapshot("S2"));
  chain.append(createSnapshot("S3"));
  
  chain.rollback("S1");
  
  StateSnapshot snap2 = chain.getSnapshot("S2");
  StateSnapshot snap3 = chain.getSnapshot("S3");
  
  EXPECT_EQ(snap2.snapshotId, "");
  EXPECT_EQ(snap3.snapshotId, "");
}
