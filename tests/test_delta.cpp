// =====================================================
// Delta Tests
// Tests for Delta<T>: change descriptor between states
// =====================================================

#include <gtest/gtest.h>
#include "types/Delta.h"
#include "types/Timestamp.h"

#include <string>
#include <vector>
#include <algorithm>

namespace ultra::types {

class DeltaTest : public ::testing::Test {
 protected:
  Delta<std::string> createDelta() {
    Delta<std::string> d;
    d.before = "old_state";
    d.after = "new_state";
    d.changeType = ChangeType::Modified;
    d.impactScore = 0.75;
    d.timestamp = Timestamp::now();
    d.description = "Updated value";
    return d;
  }
};

// ===== Basic Delta Construction =====

TEST_F(DeltaTest, DefaultDelta) {
  Delta<std::string> d;
  EXPECT_TRUE(d.before.empty());
  EXPECT_TRUE(d.after.empty());
  EXPECT_EQ(d.changeType, ChangeType::Unchanged);
  EXPECT_EQ(d.impactScore, 0.0);
  EXPECT_TRUE(d.description.empty());
}

TEST_F(DeltaTest, DeltaWithModified) {
  Delta<std::string> d = createDelta();
  EXPECT_EQ(d.before, "old_state");
  EXPECT_EQ(d.after, "new_state");
  EXPECT_EQ(d.changeType, ChangeType::Modified);
}

TEST_F(DeltaTest, DeltaWithAdded) {
  Delta<std::string> d;
  d.after = "new_item";
  d.changeType = ChangeType::Added;
  d.impactScore = 0.5;

  EXPECT_TRUE(d.before.empty());
  EXPECT_EQ(d.after, "new_item");
  EXPECT_EQ(d.changeType, ChangeType::Added);
}

TEST_F(DeltaTest, DeltaWithRemoved) {
  Delta<std::string> d;
  d.before = "old_item";
  d.changeType = ChangeType::Removed;
  d.impactScore = 0.3;

  EXPECT_EQ(d.before, "old_item");
  EXPECT_TRUE(d.after.empty());
  EXPECT_EQ(d.changeType, ChangeType::Removed);
}

// ===== Change Type Variations =====

TEST_F(DeltaTest, ChangeTypeAdded) {
  Delta<std::string> d;
  d.changeType = ChangeType::Added;
  EXPECT_EQ(d.changeType, ChangeType::Added);
}

TEST_F(DeltaTest, ChangeTypeRemoved) {
  Delta<std::string> d;
  d.changeType = ChangeType::Removed;
  EXPECT_EQ(d.changeType, ChangeType::Removed);
}

TEST_F(DeltaTest, ChangeTypeModified) {
  Delta<std::string> d;
  d.changeType = ChangeType::Modified;
  EXPECT_EQ(d.changeType, ChangeType::Modified);
}

TEST_F(DeltaTest, ChangeTypeRenamed) {
  Delta<std::string> d;
  d.changeType = ChangeType::Renamed;
  EXPECT_EQ(d.changeType, ChangeType::Renamed);
}

TEST_F(DeltaTest, ChangeTypeUnchanged) {
  Delta<std::string> d;
  d.changeType = ChangeType::Unchanged;
  EXPECT_EQ(d.changeType, ChangeType::Unchanged);
}

// ===== Change Type String Conversion =====

TEST_F(DeltaTest, ChangeTypeToStringAdded) {
  EXPECT_EQ(changeTypeToString(ChangeType::Added), "added");
}

TEST_F(DeltaTest, ChangeTypeToStringRemoved) {
  EXPECT_EQ(changeTypeToString(ChangeType::Removed), "removed");
}

TEST_F(DeltaTest, ChangeTypeToStringModified) {
  EXPECT_EQ(changeTypeToString(ChangeType::Modified), "modified");
}

TEST_F(DeltaTest, ChangeTypeToStringRenamed) {
  EXPECT_EQ(changeTypeToString(ChangeType::Renamed), "renamed");
}

TEST_F(DeltaTest, ChangeTypeToStringUnchanged) {
  EXPECT_EQ(changeTypeToString(ChangeType::Unchanged), "unchanged");
}

// ===== Impact Score Validation =====

TEST_F(DeltaTest, ImpactScoreLow) {
  Delta<std::string> d = createDelta();
  d.impactScore = 0.1;
  EXPECT_NEAR(d.impactScore, 0.1, 0.001);
}

TEST_F(DeltaTest, ImpactScoreHigh) {
  Delta<std::string> d = createDelta();
  d.impactScore = 0.95;
  EXPECT_NEAR(d.impactScore, 0.95, 0.001);
}

TEST_F(DeltaTest, ImpactScoreMid) {
  Delta<std::string> d = createDelta();
  d.impactScore = 0.5;
  EXPECT_NEAR(d.impactScore, 0.5, 0.001);
}

TEST_F(DeltaTest, ImpactScoreZero) {
  Delta<std::string> d = createDelta();
  d.impactScore = 0.0;
  EXPECT_EQ(d.impactScore, 0.0);
}

// ===== Description Field =====

TEST_F(DeltaTest, DescriptionEmpty) {
  Delta<std::string> d;
  EXPECT_TRUE(d.description.empty());
}

TEST_F(DeltaTest, DescriptionSimple) {
  Delta<std::string> d = createDelta();
  EXPECT_EQ(d.description, "Updated value");
}

TEST_F(DeltaTest, DescriptionLong) {
  Delta<std::string> d;
  std::string longDesc;
  for (int i = 0; i < 100; ++i) {
    longDesc += "description segment ";
  }
  d.description = longDesc;
  EXPECT_EQ(d.description, longDesc);
}

// ===== Delta with Different Types =====

TEST_F(DeltaTest, DeltaWithInt) {
  Delta<int> d;
  d.before = 10;
  d.after = 20;
  d.changeType = ChangeType::Modified;
  d.impactScore = 0.5;

  EXPECT_EQ(d.before, 10);
  EXPECT_EQ(d.after, 20);
  EXPECT_EQ(d.changeType, ChangeType::Modified);
}

TEST_F(DeltaTest, DeltaWithDouble) {
  Delta<double> d;
  d.before = 3.14;
  d.after = 3.14159;
  d.changeType = ChangeType::Modified;

  EXPECT_NEAR(d.before, 3.14, 0.001);
  EXPECT_NEAR(d.after, 3.14159, 0.001);
}

TEST_F(DeltaTest, DeltaWithVector) {
  Delta<std::vector<int>> d;
  d.before = {1, 2, 3};
  d.after = {1, 2, 3, 4};
  d.changeType = ChangeType::Modified;

  EXPECT_EQ(d.before.size(), 3);
  EXPECT_EQ(d.after.size(), 4);
}

// ===== Large Delta Sets =====

TEST_F(DeltaTest, LargeDeltaArray) {
  std::vector<Delta<int>> deltas;
  for (int i = 0; i < 100; ++i) {
    Delta<int> d;
    d.before = i;
    d.after = i * 2;
    d.changeType = ChangeType::Modified;
    d.impactScore = 0.5 + (i % 5) * 0.1;
    deltas.push_back(d);
  }

  EXPECT_EQ(deltas.size(), 100);
  EXPECT_EQ(deltas[50].before, 50);
  EXPECT_EQ(deltas[50].after, 100);
}

TEST_F(DeltaTest, LargeDeltaWithLongStrings) {
  std::string longStr;
  for (int i = 0; i < 500; ++i) {
    longStr += "x";
  }

  Delta<std::string> d;
  d.before = longStr;
  d.after = longStr + "y";
  d.changeType = ChangeType::Modified;

  EXPECT_EQ(d.before.length(), 500);
  EXPECT_EQ(d.after.length(), 501);
}

// ===== Timestamp Handling =====

TEST_F(DeltaTest, TimestampDefault) {
  Delta<std::string> d;
  EXPECT_GE(d.timestamp.epochMs(), 0);
}

TEST_F(DeltaTest, TimestampNow) {
  auto before = Timestamp::now();
  Delta<std::string> d = createDelta();
  auto after = Timestamp::now();

  EXPECT_LE(before.epochMs(), d.timestamp.epochMs());
  EXPECT_LE(d.timestamp.epochMs(), after.epochMs());
}

TEST_F(DeltaTest, TimestampCustom) {
  Delta<std::string> d;
  d.timestamp = Timestamp::fromEpochMs(1000000000000);
  EXPECT_EQ(d.timestamp.epochMs(), 1000000000000);
}

// ===== Deterministic Ordering =====

TEST_F(DeltaTest, DeterministicDeltaCreation) {
  for (int i = 0; i < 5; ++i) {
    Delta<std::string> d = createDelta();
    EXPECT_EQ(d.before, "old_state");
    EXPECT_EQ(d.after, "new_state");
    EXPECT_EQ(d.changeType, ChangeType::Modified);
    EXPECT_NEAR(d.impactScore, 0.75, 0.001);
  }
}

// ===== Duplicate Entry Handling =====

TEST_F(DeltaTest, DuplicateDeltaInSet) {
  std::vector<Delta<std::string>> deltas;

  Delta<std::string> d1 = createDelta();
  Delta<std::string> d2 = createDelta();

  deltas.push_back(d1);
  deltas.push_back(d2);

  EXPECT_EQ(deltas.size(), 2);
  EXPECT_EQ(deltas[0].before, deltas[1].before);
  EXPECT_EQ(deltas[0].after, deltas[1].after);
}

TEST_F(DeltaTest, DeltasWithSameStateButDifferentDescription) {
  Delta<std::string> d1;
  d1.before = "old";
  d1.after = "new";
  d1.changeType = ChangeType::Modified;
  d1.description = "Description 1";

  Delta<std::string> d2;
  d2.before = "old";
  d2.after = "new";
  d2.changeType = ChangeType::Modified;
  d2.description = "Description 2";

  EXPECT_EQ(d1.before, d2.before);
  EXPECT_EQ(d1.after, d2.after);
  EXPECT_NE(d1.description, d2.description);
}

// ===== Stability Across Operations =====

TEST_F(DeltaTest, StabilityMultipleAccess) {
  Delta<std::string> d = createDelta();

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(d.before, "old_state");
    EXPECT_EQ(d.after, "new_state");
    EXPECT_EQ(d.changeType, ChangeType::Modified);
  }
}

TEST_F(DeltaTest, StabilityAfterModification) {
  Delta<std::string> d = createDelta();

  d.before = "modified_old";
  EXPECT_EQ(d.before, "modified_old");

  d.before = "modified_old";
  EXPECT_EQ(d.before, "modified_old");
}

// ===== Edge Cases =====

TEST_F(DeltaTest, DeltaWithEmptyBefore) {
  Delta<std::string> d;
  d.before = "";
  d.after = "value";
  d.changeType = ChangeType::Added;

  EXPECT_TRUE(d.before.empty());
  EXPECT_EQ(d.after, "value");
}

TEST_F(DeltaTest, DeltaWithEmptyAfter) {
  Delta<std::string> d;
  d.before = "value";
  d.after = "";
  d.changeType = ChangeType::Removed;

  EXPECT_EQ(d.before, "value");
  EXPECT_TRUE(d.after.empty());
}

TEST_F(DeltaTest, DeltaWithZeroImpact) {
  Delta<std::string> d;
  d.before = "old";
  d.after = "new";
  d.changeType = ChangeType::Modified;
  d.impactScore = 0.0;

  EXPECT_EQ(d.impactScore, 0.0);
}

TEST_F(DeltaTest, DeltaWithMaxImpact) {
  Delta<std::string> d;
  d.before = "old";
  d.after = "new";
  d.changeType = ChangeType::Modified;
  d.impactScore = 1.0;

  EXPECT_EQ(d.impactScore, 1.0);
}

TEST_F(DeltaTest, DeltaWithSameBeforeAndAfter) {
  Delta<std::string> d;
  d.before = "same";
  d.after = "same";
  d.changeType = ChangeType::Unchanged;

  EXPECT_EQ(d.before, d.after);
  EXPECT_EQ(d.changeType, ChangeType::Unchanged);
}

TEST_F(DeltaTest, DeltaRenamedType) {
  Delta<std::string> d;
  d.before = "old_name";
  d.after = "new_name";
  d.changeType = ChangeType::Renamed;

  EXPECT_EQ(changeTypeToString(d.changeType), "renamed");
}

}  // namespace ultra::types
