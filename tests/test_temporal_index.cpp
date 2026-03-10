// ============================================================================
// File: tests/test_temporal_index.cpp
// Deterministic ID-based snapshot queries
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "memory/TemporalIndex.h"
#include "memory/SnapshotChain.h"
#include "memory/StateSnapshot.h"

using namespace ultra::memory;

class TemporalIndexTest : public ::testing::Test {
 protected:
  SnapshotChain chain;

  StateSnapshot createSnapshot(uint64_t id) {
    StateSnapshot snap;
    snap.id = id;
    snap.snapshotId = std::to_string(id);
    snap.nodeCount = id;
    snap.edgeCount = id;
    snap.graphHash = "hash_" + std::to_string(id);
    return snap;
  }

  bool containsSnapshot(const std::vector<StateSnapshot>& snaps,
                        uint64_t id) {
    return std::any_of(
        snaps.begin(), snaps.end(),
        [&](const StateSnapshot& s) { return s.id == id; });
  }
};

TEST_F(TemporalIndexTest, QueryExactId) {
  chain.append(createSnapshot(1));
  chain.append(createSnapshot(2));

  TemporalIndex index(chain);

  StateSnapshot snap = index.queryAtId(2);
  EXPECT_EQ(snap.id, 2);
}

TEST_F(TemporalIndexTest, QueryBeforeFirst) {
  chain.append(createSnapshot(10));
  chain.append(createSnapshot(20));

  TemporalIndex index(chain);

  StateSnapshot snap = index.queryAtId(5);
  EXPECT_EQ(snap.id, 0);
}

TEST_F(TemporalIndexTest, QueryAfterLast) {
  chain.append(createSnapshot(1));
  chain.append(createSnapshot(2));

  TemporalIndex index(chain);

  StateSnapshot snap = index.queryAtId(100);
  EXPECT_EQ(snap.id, 2);
}

TEST_F(TemporalIndexTest, QueryBetweenIds) {
  chain.append(createSnapshot(1));
  chain.append(createSnapshot(5));

  TemporalIndex index(chain);

  StateSnapshot snap = index.queryAtId(3);
  EXPECT_EQ(snap.id, 1);
}

TEST_F(TemporalIndexTest, RangeQueryBasic) {
  for (uint64_t i = 1; i <= SnapshotChain::kMaxSnapshotsRetained; ++i)
    chain.append(createSnapshot(i));

  TemporalIndex index(chain);

  auto results = index.getChangesBetween(2, 3);

  EXPECT_EQ(results.size(), 2);
  EXPECT_TRUE(containsSnapshot(results, 2U));
  EXPECT_TRUE(containsSnapshot(results, 3U));
}

TEST_F(TemporalIndexTest, RangeQueryEmpty) {
  chain.append(createSnapshot(10));
  chain.append(createSnapshot(20));

  TemporalIndex index(chain);

  auto results = index.getChangesBetween(30, 40);
  EXPECT_TRUE(results.empty());
}

TEST_F(TemporalIndexTest, EmptyChain) {
  TemporalIndex index(chain);

  StateSnapshot snap = index.queryAtId(10);
  EXPECT_EQ(snap.id, 0);

  auto results = index.getChangesBetween(1, 10);
  EXPECT_TRUE(results.empty());
}

TEST_F(TemporalIndexTest, LargeInsertion) {
  chain.append(createSnapshot(10));
  chain.append(createSnapshot(20));
  chain.append(createSnapshot(30));

  TemporalIndex index(chain);

  StateSnapshot snap = index.queryAtId(25);
  EXPECT_EQ(snap.id, 20);
}

TEST_F(TemporalIndexTest, OrderingStability) {
  for (uint64_t i = 1; i <= 5; ++i)
    chain.append(createSnapshot(i));

  TemporalIndex index(chain);

  auto r1 = index.getChangesBetween(2, 4);
  auto r2 = index.getChangesBetween(2, 4);

  ASSERT_EQ(r1.size(), r2.size());
  for (size_t i = 0; i < r1.size(); ++i)
    EXPECT_EQ(r1[i].id, r2[i].id);
}

TEST_F(TemporalIndexTest, DeterministicRepeatedQuery) {
  chain.append(createSnapshot(5));
  chain.append(createSnapshot(7));
  chain.append(createSnapshot(9));

  TemporalIndex index(chain);

  for (int i = 0; i < 100; ++i) {
    StateSnapshot snap = index.queryAtId(8);
    EXPECT_EQ(snap.id, 7);
  }
}
