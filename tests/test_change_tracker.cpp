// ============================================================================
// File: tests/incremental/test_change_tracker.cpp
// Tests for ChangeTracker changelog recording
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include "ai/ChangeTracker.h"

using namespace ultra::ai;
namespace fs = std::filesystem;

class ChangeTrackerTest : public ::testing::Test {
 protected:
  fs::path testDir;
  fs::path logPath;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "change_tracker";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
    logPath = testDir / "changes.log";
  }

  void TearDown() override {
    fs::remove_all(testDir);
  }

  ChangeLogRecord createRecord(std::uint32_t fileId,
                              ChangeType type = ChangeType::Added,
                              std::uint64_t timestamp = 0) {
    ChangeLogRecord rec;
    rec.fileId = fileId;
    rec.changeType = type;
    rec.timestamp = timestamp;
    return rec;
  }
};

TEST_F(ChangeTrackerTest, AppendSingleRecord) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(1, ChangeType::Added, 1000));
  
  std::string error;
  bool success = ChangeTracker::append(logPath, records, error);
  
  EXPECT_TRUE(success);
  EXPECT_TRUE(fs::exists(logPath));
}

TEST_F(ChangeTrackerTest, AppendMultipleRecords) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(1, ChangeType::Added, 1000));
  records.push_back(createRecord(2, ChangeType::Modified, 2000));
  records.push_back(createRecord(3, ChangeType::Deleted, 3000));
  
  std::string error;
  bool success = ChangeTracker::append(logPath, records, error);
  
  EXPECT_TRUE(success);
}

TEST_F(ChangeTrackerTest, ReadEmptyLog) {
  std::vector<ChangeLogRecord> records;
  std::string error;
  
  bool success = ChangeTracker::readAll(logPath, records, error);
  
  EXPECT_TRUE(success);
  EXPECT_EQ(records.size(), 0);
}

TEST_F(ChangeTrackerTest, AppendThenRead) {
  std::vector<ChangeLogRecord> originals;
  originals.push_back(createRecord(1, ChangeType::Added, 1000));
  originals.push_back(createRecord(2, ChangeType::Modified, 2000));
  
  std::string error;
  ChangeTracker::append(logPath, originals, error);
  
  std::vector<ChangeLogRecord> readRecords;
  bool success = ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_TRUE(success);
  EXPECT_EQ(readRecords.size(), 2);
  EXPECT_EQ(readRecords[0].fileId, 1);
  EXPECT_EQ(readRecords[1].fileId, 2);
}

TEST_F(ChangeTrackerTest, SequentialAppends) {
  std::string error;
  
  std::vector<ChangeLogRecord> records1;
  records1.push_back(createRecord(1, ChangeType::Added, 1000));
  ChangeTracker::append(logPath, records1, error);
  
  std::vector<ChangeLogRecord> records2;
  records2.push_back(createRecord(2, ChangeType::Modified, 2000));
  ChangeTracker::append(logPath, records2, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords.size(), 2);
}

TEST_F(ChangeTrackerTest, ChangeTypeAddedDetection) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(1, ChangeType::Added, 1000));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].changeType, ChangeType::Added);
}

TEST_F(ChangeTrackerTest, ChangeTypeModifiedDetection) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(1, ChangeType::Modified, 2000));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].changeType, ChangeType::Modified);
}

TEST_F(ChangeTrackerTest, ChangeTypeDeletedDetection) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(1, ChangeType::Deleted, 3000));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].changeType, ChangeType::Deleted);
}

TEST_F(ChangeTrackerTest, TimestampPreservation) {
  std::vector<ChangeLogRecord> records;
  std::uint64_t ts1 = 1234567890;
  std::uint64_t ts2 = 9876543210;
  records.push_back(createRecord(1, ChangeType::Added, ts1));
  records.push_back(createRecord(2, ChangeType::Modified, ts2));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].timestamp, ts1);
  EXPECT_EQ(readRecords[1].timestamp, ts2);
}

TEST_F(ChangeTrackerTest, LargeFileIdHandling) {
  std::vector<ChangeLogRecord> records;
  std::uint32_t largeId = 0xFFFFFFFF;
  records.push_back(createRecord(largeId, ChangeType::Added, 1000));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].fileId, largeId);
}

TEST_F(ChangeTrackerTest, LargeTimestampHandling) {
  std::vector<ChangeLogRecord> records;
  std::uint64_t largeTs = 0xFFFFFFFFFFFFFFFF;
  records.push_back(createRecord(1, ChangeType::Added, largeTs));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].timestamp, largeTs);
}

TEST_F(ChangeTrackerTest, AppendEmptyVector) {
  std::vector<ChangeLogRecord> empty;
  std::string error;
  
  bool success = ChangeTracker::append(logPath, empty, error);
  
  EXPECT_TRUE(success);
}

TEST_F(ChangeTrackerTest, MultipleRecordTypes) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(1, ChangeType::Added, 1000));
  records.push_back(createRecord(2, ChangeType::Modified, 2000));
  records.push_back(createRecord(3, ChangeType::Deleted, 3000));
  records.push_back(createRecord(4, ChangeType::Added, 4000));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords.size(), 4);
  EXPECT_EQ(readRecords[0].changeType, ChangeType::Added);
  EXPECT_EQ(readRecords[1].changeType, ChangeType::Modified);
  EXPECT_EQ(readRecords[2].changeType, ChangeType::Deleted);
  EXPECT_EQ(readRecords[3].changeType, ChangeType::Added);
}

TEST_F(ChangeTrackerTest, DeterministicRecordStorage) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(42, ChangeType::Modified, 5678));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> read1;
  ChangeTracker::readAll(logPath, read1, error);
  
  std::vector<ChangeLogRecord> read2;
  ChangeTracker::readAll(logPath, read2, error);
  
  EXPECT_EQ(read1.size(), read2.size());
  EXPECT_EQ(read1[0].fileId, read2[0].fileId);
  EXPECT_EQ(read1[0].changeType, read2[0].changeType);
  EXPECT_EQ(read1[0].timestamp, read2[0].timestamp);
}

TEST_F(ChangeTrackerTest, ManyRecords) {
  std::vector<ChangeLogRecord> records;
  for (std::uint32_t i = 0; i < 100; ++i) {
    records.push_back(createRecord(i, ChangeType::Added, i * 100));
  }
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords.size(), 100);
}

TEST_F(ChangeTrackerTest, SequentialAppendReadAppend) {
  std::string error;
  
  std::vector<ChangeLogRecord> records1;
  records1.push_back(createRecord(1, ChangeType::Added, 1000));
  ChangeTracker::append(logPath, records1, error);
  
  std::vector<ChangeLogRecord> read1;
  ChangeTracker::readAll(logPath, read1, error);
  EXPECT_EQ(read1.size(), 1);
  
  std::vector<ChangeLogRecord> records2;
  records2.push_back(createRecord(2, ChangeType::Modified, 2000));
  ChangeTracker::append(logPath, records2, error);
  
  std::vector<ChangeLogRecord> read2;
  ChangeTracker::readAll(logPath, read2, error);
  EXPECT_EQ(read2.size(), 2);
}

TEST_F(ChangeTrackerTest, ZeroTimestamp) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(1, ChangeType::Added, 0));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].timestamp, 0);
}

TEST_F(ChangeTrackerTest, ZeroFileId) {
  std::vector<ChangeLogRecord> records;
  records.push_back(createRecord(0, ChangeType::Added, 1000));
  
  std::string error;
  ChangeTracker::append(logPath, records, error);
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords[0].fileId, 0);
}

TEST_F(ChangeTrackerTest, ConsecutiveAppends) {
  std::string error;
  
  for (int i = 0; i < 5; ++i) {
    std::vector<ChangeLogRecord> records;
    records.push_back(createRecord(i, ChangeType::Added, i * 1000));
    ChangeTracker::append(logPath, records, error);
  }
  
  std::vector<ChangeLogRecord> readRecords;
  ChangeTracker::readAll(logPath, readRecords, error);
  
  EXPECT_EQ(readRecords.size(), 5);
}

TEST_F(ChangeTrackerTest, ReadNonexistentLog) {
  std::vector<ChangeLogRecord> records;
  std::string error;
  
  fs::path nonexistent = testDir / "nonexistent.log";
  bool success = ChangeTracker::readAll(nonexistent, records, error);
  
  EXPECT_TRUE(success);
  EXPECT_EQ(records.size(), 0);
}
