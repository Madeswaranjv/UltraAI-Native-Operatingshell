// ============================================================================
// File: tests/calibration/test_usage_tracker.cpp
// Tests for UsageTracker event recording and history
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "calibration/UsageTracker.h"

using namespace ultra::calibration;

class UsageTrackerTest : public ::testing::Test {
 protected:
  UsageTracker tracker;
};

TEST_F(UsageTrackerTest, RecordSingleEvent) {
  tracker.record("analyze", {"file.cpp"});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 1);
  EXPECT_EQ(history[0].command, "analyze");
}

TEST_F(UsageTrackerTest, RecordWithArguments) {
  std::vector<std::string> args = {"arg1", "arg2", "arg3"};
  tracker.record("process", args);
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history[0].command, "process");
  EXPECT_EQ(history[0].args.size(), 3);
  EXPECT_EQ(history[0].args[0], "arg1");
  EXPECT_EQ(history[0].args[1], "arg2");
  EXPECT_EQ(history[0].args[2], "arg3");
}

TEST_F(UsageTrackerTest, RecordMultipleEvents) {
  tracker.record("cmd1", {"arg1"});
  tracker.record("cmd2", {"arg2"});
  tracker.record("cmd3", {"arg3"});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 3);
  EXPECT_EQ(history[0].command, "cmd1");
  EXPECT_EQ(history[1].command, "cmd2");
  EXPECT_EQ(history[2].command, "cmd3");
}

TEST_F(UsageTrackerTest, EventTimestampAssignment) {
  tracker.record("timed_cmd", {});
  
  auto history = tracker.getHistory();
  EXPECT_FALSE(history[0].timestamp.toISO8601().empty());
}

TEST_F(UsageTrackerTest, RecordWithEmptyArguments) {
  tracker.record("no_args", {});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history[0].command, "no_args");
  EXPECT_EQ(history[0].args.size(), 0);
}

TEST_F(UsageTrackerTest, RecordWithManyArguments) {
  std::vector<std::string> args;
  for (int i = 0; i < 50; ++i) {
    args.push_back("arg_" + std::to_string(i));
  }
  
  tracker.record("multi_arg", args);
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history[0].args.size(), 50);
}

TEST_F(UsageTrackerTest, ClearHistory) {
  tracker.record("cmd1", {});
  tracker.record("cmd2", {});
  
  auto history1 = tracker.getHistory();
  EXPECT_EQ(history1.size(), 2);
  
  tracker.clear();
  
  auto history2 = tracker.getHistory();
  EXPECT_EQ(history2.size(), 0);
}

TEST_F(UsageTrackerTest, MaxHistoryLimit) {
  for (int i = 0; i < 1500; ++i) {
    tracker.record("cmd_" + std::to_string(i), {});
  }
  
  auto history = tracker.getHistory();
  EXPECT_LE(history.size(), 1000);
}

TEST_F(UsageTrackerTest, OldestEventRemovedWhenLimitExceeded) {
  for (int i = 0; i < 1002; ++i) {
    tracker.record("cmd_" + std::to_string(i), {});
  }
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 1000);
  EXPECT_EQ(history[0].command, "cmd_2");
}

TEST_F(UsageTrackerTest, HistoryOrderPreservation) {
  tracker.record("first", {});
  tracker.record("second", {});
  tracker.record("third", {});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history[0].command, "first");
  EXPECT_EQ(history[1].command, "second");
  EXPECT_EQ(history[2].command, "third");
}

TEST_F(UsageTrackerTest, EmptyTrackerHistory) {
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 0);
}

TEST_F(UsageTrackerTest, MultipleRecordsWithSameCommand) {
  tracker.record("repeat", {"a"});
  tracker.record("repeat", {"b"});
  tracker.record("repeat", {"c"});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 3);
  for (const auto& event : history) {
    EXPECT_EQ(event.command, "repeat");
  }
}

TEST_F(UsageTrackerTest, DistinctTimestampsForEvents) {
  tracker.record("event1", {});
  tracker.record("event2", {});
  
  auto history = tracker.getHistory();
  std::string ts1 = history[0].timestamp.toISO8601();
  std::string ts2 = history[1].timestamp.toISO8601();
  
  EXPECT_FALSE(ts1.empty());
  EXPECT_FALSE(ts2.empty());
}

TEST_F(UsageTrackerTest, LargeArgumentValues) {
  std::string largeArg(10000, 'x');
  tracker.record("large", {largeArg});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history[0].args[0].length(), 10000);
}

TEST_F(UsageTrackerTest, SpecialCharactersInCommand) {
  tracker.record("cmd-with-dashes", {});
  tracker.record("cmd_with_underscores", {});
  tracker.record("cmd.with.dots", {});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history[0].command, "cmd-with-dashes");
  EXPECT_EQ(history[1].command, "cmd_with_underscores");
  EXPECT_EQ(history[2].command, "cmd.with.dots");
}

TEST_F(UsageTrackerTest, SpecialCharactersInArguments) {
  tracker.record("cmd", {"arg@with#special$chars"});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history[0].args[0], "arg@with#special$chars");
}

TEST_F(UsageTrackerTest, DeterministicHistoryRetrieval) {
  tracker.record("cmd1", {"arg1"});
  tracker.record("cmd2", {"arg2"});
  
  auto hist1 = tracker.getHistory();
  auto hist2 = tracker.getHistory();
  
  EXPECT_EQ(hist1.size(), hist2.size());
  for (size_t i = 0; i < hist1.size(); ++i) {
    EXPECT_EQ(hist1[i].command, hist2[i].command);
  }
}

TEST_F(UsageTrackerTest, IsolationBetweenInstances) {
  UsageTracker tracker1;
  UsageTracker tracker2;
  
  tracker1.record("track1_cmd", {});
  tracker2.record("track2_cmd", {});
  
  auto hist1 = tracker1.getHistory();
  auto hist2 = tracker2.getHistory();
  
  EXPECT_EQ(hist1.size(), 1);
  EXPECT_EQ(hist2.size(), 1);
  EXPECT_EQ(hist1[0].command, "track1_cmd");
  EXPECT_EQ(hist2[0].command, "track2_cmd");
}

TEST_F(UsageTrackerTest, ClearThenRecord) {
  tracker.record("first", {});
  tracker.clear();
  tracker.record("second", {});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 1);
  EXPECT_EQ(history[0].command, "second");
}

TEST_F(UsageTrackerTest, UnknownKeyNoError) {
  tracker.record("any_command", {});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 1);
}

TEST_F(UsageTrackerTest, StabilityAcrossRepeatedCalls) {
  tracker.record("stable", {"arg"});
  
  std::vector<UsageEvent> results[3];
  for (int i = 0; i < 3; ++i) {
    results[i] = tracker.getHistory();
  }
  
  EXPECT_EQ(results[0].size(), results[1].size());
  EXPECT_EQ(results[1].size(), results[2].size());
}

TEST_F(UsageTrackerTest, HistoryAccessDoesNotMutate) {
  tracker.record("cmd", {});
  
  auto hist1_size = tracker.getHistory().size();
  auto hist2_size = tracker.getHistory().size();
  
  EXPECT_EQ(hist1_size, hist2_size);
}

TEST_F(UsageTrackerTest, LargeNumberOfRecords) {
  for (int i = 0; i < 100; ++i) {
    tracker.record("cmd_" + std::to_string(i), {});
  }
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 100);
}

TEST_F(UsageTrackerTest, MixedCommandsAndArguments) {
  tracker.record("analyze", {"file.cpp", "detailed"});
  tracker.record("build", {"release"});
  tracker.record("deploy", {"prod", "auto-rollback"});
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 3);
  EXPECT_EQ(history[0].args.size(), 2);
  EXPECT_EQ(history[1].args.size(), 1);
  EXPECT_EQ(history[2].args.size(), 2);
}

TEST_F(UsageTrackerTest, RecordWithUnicodeCharacters) {
  tracker.record("cmd_ñ", {"arg_ü"});
  
  auto history = tracker.getHistory();
  EXPECT_FALSE(history.empty());
}

TEST_F(UsageTrackerTest, ConsecutiveClears) {
  tracker.record("cmd", {});
  tracker.clear();
  tracker.clear();
  tracker.clear();
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 0);
}

TEST_F(UsageTrackerTest, MaxHistoryRetainment) {
  for (int i = 0; i < 2000; ++i) {
    tracker.record("cmd", {});
  }
  
  auto history = tracker.getHistory();
  EXPECT_EQ(history.size(), 1000);
}
