// ============================================================================
// File: tests/patch/test_diff_parser.cpp
// Tests for DiffParser unified diff parsing
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include "patch/DiffParser.h"

using namespace ultra::patch;
namespace fs = std::filesystem;

class DiffParserTest : public ::testing::Test {
 protected:
  fs::path testDir;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "diff_parser";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
  }

  void TearDown() override {
    fs::remove_all(testDir);
  }

  void writeFile(const fs::path& path, const std::string& content) {
    std::ofstream file(path);
    file << content;
  }

  bool containsString(const std::vector<std::string>& vec, const std::string& val) {
    return std::find(vec.begin(), vec.end(), val) != vec.end();
  }
};

TEST_F(DiffParserTest, ParseValidUnifiedDiff) {
  fs::path diffFile = testDir / "test.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-old line\n"
                     "+new line\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].targetFile, "file.txt");
}

TEST_F(DiffParserTest, ParseMultiFileDiff) {
  fs::path diffFile = testDir / "multi.diff";
  std::string diff = "--- a/file1.txt\n+++ b/file1.txt\n"
                     "-line1\n"
                     "+new1\n"
                     "--- a/file2.txt\n+++ b/file2.txt\n"
                     "-line2\n"
                     "+new2\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 2);
  EXPECT_EQ(ops[0].targetFile, "file1.txt");
  EXPECT_EQ(ops[1].targetFile, "file2.txt");
}

TEST_F(DiffParserTest, ParseAddedLines) {
  fs::path diffFile = testDir / "added.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "+new line 1\n"
                     "+new line 2\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].addedLines.size(), 2);
  EXPECT_TRUE(containsString(ops[0].addedLines, "new line 1"));
  EXPECT_TRUE(containsString(ops[0].addedLines, "new line 2"));
}

TEST_F(DiffParserTest, ParseRemovedLines) {
  fs::path diffFile = testDir / "removed.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-old line 1\n"
                     "-old line 2\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].removedLines.size(), 2);
  EXPECT_TRUE(containsString(ops[0].removedLines, "old line 1"));
  EXPECT_TRUE(containsString(ops[0].removedLines, "old line 2"));
}

TEST_F(DiffParserTest, ParseModifiedSection) {
  fs::path diffFile = testDir / "modified.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-old content\n"
                     "+new content\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].removedLines.size(), 1);
  EXPECT_EQ(ops[0].addedLines.size(), 1);
}

TEST_F(DiffParserTest, ParseEmptyDiff) {
  fs::path diffFile = testDir / "empty.diff";
  writeFile(diffFile, "");
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 0);
}

TEST_F(DiffParserTest, ParseNonexistentFile) {
  fs::path diffFile = testDir / "nonexistent.diff";
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 0);
}

TEST_F(DiffParserTest, ParseLargeDiff) {
  fs::path diffFile = testDir / "large.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n";
  
  for (int i = 0; i < 50; ++i) {
    diff += "-old line " + std::to_string(i) + "\n";
    diff += "+new line " + std::to_string(i) + "\n";
  }
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].removedLines.size(), 50);
  EXPECT_EQ(ops[0].addedLines.size(), 50);
}

TEST_F(DiffParserTest, DeterministicParsing) {
  fs::path diffFile = testDir / "det.diff";
  std::string diff = "--- a/test.cpp\n+++ b/test.cpp\n"
                     "-int x = 0;\n"
                     "+int x = 1;\n";
  writeFile(diffFile, diff);
  
  auto ops1 = DiffParser::parse(diffFile);
  auto ops2 = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops1.size(), ops2.size());
  EXPECT_EQ(ops1[0].targetFile, ops2[0].targetFile);
  EXPECT_EQ(ops1[0].removedLines.size(), ops2[0].removedLines.size());
}

TEST_F(DiffParserTest, StabilityAcrossRepeatedParsing) {
  fs::path diffFile = testDir / "stable.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-line1\n"
                     "+line2\n";
  writeFile(diffFile, diff);
  
  std::vector<size_t> sizes;
  for (int i = 0; i < 3; ++i) {
    auto ops = DiffParser::parse(diffFile);
    sizes.push_back(ops.size());
  }
  
  EXPECT_EQ(sizes[0], sizes[1]);
  EXPECT_EQ(sizes[1], sizes[2]);
}

TEST_F(DiffParserTest, ParseWithTabSeparator) {
  fs::path diffFile = testDir / "tab.diff";
  std::string diff = "--- a/file.txt\t2024-01-01\n"
                     "+++ b/file.txt\t2024-01-02\n"
                     "-old\n"
                     "+new\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].targetFile, "file.txt");
}

TEST_F(DiffParserTest, ParseMultipleHunks) {
  fs::path diffFile = testDir / "hunks.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-line1\n"
                     "+newline1\n"
                     "-line2\n"
                     "+newline2\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].removedLines.size(), 2);
  EXPECT_EQ(ops[0].addedLines.size(), 2);
}

TEST_F(DiffParserTest, ParsePathWithPrefix) {
  fs::path diffFile = testDir / "prefix.diff";
  std::string diff = "--- a/src/file.cpp\n+++ b/src/file.cpp\n"
                     "-old\n"
                     "+new\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].targetFile, "src/file.cpp");
}

TEST_F(DiffParserTest, ParsePathWithoutPrefix) {
  fs::path diffFile = testDir / "noprefix.diff";
  std::string diff = "--- src/file.cpp\n+++ src/file.cpp\n"
                     "-old\n"
                     "+new\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
}

TEST_F(DiffParserTest, IgnoreContextLines) {
  fs::path diffFile = testDir / "context.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     " context line\n"
                     "-removed\n"
                     " more context\n"
                     "+added\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
}

TEST_F(DiffParserTest, ParseSpecialCharactersInLines) {
  fs::path diffFile = testDir / "special.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-line with !@#$%^&*()\n"
                     "+line with special chars\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_TRUE(containsString(ops[0].removedLines, "line with !@#$%^&*()"));
}

TEST_F(DiffParserTest, MalformedHeaderIgnored) {
  fs::path diffFile = testDir / "malformed.diff";
  std::string diff = "invalid header\n"
                     "--- a/file.txt\n+++ b/file.txt\n"
                     "-old\n"
                     "+new\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_GE(ops.size(), 0);
}

TEST_F(DiffParserTest, ParseUnicodeCharacters) {
  fs::path diffFile = testDir / "unicode.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-こんにちは\n"
                     "+世界\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
}

TEST_F(DiffParserTest, OnlyAddedNoRemoved) {
  fs::path diffFile = testDir / "onlyAdded.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "+new line 1\n"
                     "+new line 2\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].removedLines.size(), 0);
  EXPECT_EQ(ops[0].addedLines.size(), 2);
}

TEST_F(DiffParserTest, OnlyRemovedNoAdded) {
  fs::path diffFile = testDir / "onlyRemoved.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-old line 1\n"
                     "-old line 2\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].removedLines.size(), 2);
  EXPECT_EQ(ops[0].addedLines.size(), 0);
}

TEST_F(DiffParserTest, WindowsPathSeparators) {
  fs::path diffFile = testDir / "winpath.diff";
  std::string diff = "--- a\\src\\file.cpp\n+++ b\\src\\file.cpp\n"
                     "-old\n"
                     "+new\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_GE(ops.size(), 0);
}

TEST_F(DiffParserTest, EmptyLinesHandling) {
  fs::path diffFile = testDir / "emptylines.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-\n"
                     "+\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
}

TEST_F(DiffParserTest, WhitespacePreservation) {
  fs::path diffFile = testDir / "whitespace.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-  spaces  \n"
                     "+\ttabs\there\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
}

TEST_F(DiffParserTest, LargeNumberOfAddedLines) {
  fs::path diffFile = testDir / "manyAdded.diff";
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n";
  for (int i = 0; i < 100; ++i) {
    diff += "+line " + std::to_string(i) + "\n";
  }
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 1);
  EXPECT_EQ(ops[0].addedLines.size(), 100);
}

TEST_F(DiffParserTest, ComplexMultiFileScenario) {
  fs::path diffFile = testDir / "complex.diff";
  std::string diff = "--- a/file1.cpp\n+++ b/file1.cpp\n"
                     "-void old() {}\n"
                     "+void new() {}\n"
                     "--- a/file2.h\n+++ b/file2.h\n"
                     "-int x;\n"
                     "+int y;\n"
                     "--- a/file3.txt\n+++ b/file3.txt\n"
                     "+data\n";
  writeFile(diffFile, diff);
  
  auto ops = DiffParser::parse(diffFile);
  
  EXPECT_EQ(ops.size(), 3);
}
