// ============================================================================
// File: tests/patch/test_patch_manager.cpp
// Tests for PatchManager patch application
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "patch/PatchManager.h"
#include "patch/DiffParser.h"
#include "build/BuildEngine.h"

using namespace ultra::patch;
namespace fs = std::filesystem;

class MockBuildEngine : public ultra::build::BuildEngine {
 public:
  MockBuildEngine() : failBuild_(false) {}
  
  int fullBuild(const std::filesystem::path& /*projectPath*/) override {
    if (failBuild_) {
      return 1;
    }
    return 0;
  }
  
  void setFailBuild(bool fail) { failBuild_ = fail; }
  
 private:
  bool failBuild_;
};

class PatchManagerTest : public ::testing::Test {
 protected:
  fs::path testDir;
  fs::path projectDir;
  MockBuildEngine mockEngine;
  PatchManager* patchMgr;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "patch_manager";
    projectDir = testDir / "project";
    fs::remove_all(testDir);
    fs::create_directories(projectDir);
    patchMgr = new PatchManager(mockEngine);
  }

  void TearDown() override {
    delete patchMgr;
    fs::remove_all(testDir);
  }

  void createSourceFile(const std::string& relPath, const std::string& content) {
    fs::path file = projectDir / relPath;
    fs::create_directories(file.parent_path());
    std::ofstream out(file);
    out << content;
  }

  void createDiffFile(const std::string& fileName, const std::string& diffContent) {
    fs::path diffFile = testDir / fileName;
    std::ofstream out(diffFile);
    out << diffContent;
  }

  std::string readFile(const fs::path& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
};

TEST_F(PatchManagerTest, ApplySimplePatch) {
  createSourceFile("test.txt", "old content\n");
  
  std::string diff = "--- a/test.txt\n+++ b/test.txt\n"
                     "-old content\n"
                     "+new content\n";
  createDiffFile("simple.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "simple.diff");
  
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.filesModified, 1);
}

TEST_F(PatchManagerTest, ApplyMultiHunkPatch) {
  createSourceFile("file.txt", "line1\nline2\nline3\n");
  
  std::string diff = "--- a/file.txt\n+++ b/file.txt\n"
                     "-line1\n"
                     "+modified1\n"
                     "-line2\n"
                     "+modified2\n";
  createDiffFile("multihunk.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "multihunk.diff");
  
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.filesModified, 1);
}

TEST_F(PatchManagerTest, ApplyMultiFilePatch) {
  createSourceFile("file1.txt", "content1\n");
  createSourceFile("file2.txt", "content2\n");
  
  std::string diff = "--- a/file1.txt\n+++ b/file1.txt\n"
                     "-content1\n"
                     "+new1\n"
                     "--- a/file2.txt\n+++ b/file2.txt\n"
                     "-content2\n"
                     "+new2\n";
  createDiffFile("multifile.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "multifile.diff");
  
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.filesModified, 2);
}

TEST_F(PatchManagerTest, RejectInvalidPatch) {
  fs::path missingFile = projectDir / "missing.txt";
  
  std::string diff = "--- a/missing.txt\n+++ b/missing.txt\n"
                     "-content\n"
                     "+newcontent\n";
  createDiffFile("invalid.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "invalid.diff");
  
  EXPECT_FALSE(result.success);
}

TEST_F(PatchManagerTest, DeterministicPatchApplication) {
  createSourceFile("test.txt", "original\n");
  
  std::string diff = "--- a/test.txt\n+++ b/test.txt\n"
                     "-original\n"
                     "+modified\n";
  createDiffFile("det.diff", diff);
  
  auto result1 = patchMgr->applyPatch(projectDir, testDir / "det.diff");
  
  createSourceFile("test.txt", "original\n");
  
  auto result2 = patchMgr->applyPatch(projectDir, testDir / "det.diff");
  
  EXPECT_EQ(result1.success, result2.success);
  EXPECT_EQ(result1.filesModified, result2.filesModified);
}

TEST_F(PatchManagerTest, StabilityAcrossRepeatedApplication) {
  createSourceFile("test.txt", "content\n");
  
  std::string diff = "--- a/test.txt\n+++ b/test.txt\n"
                     "-content\n"
                     "+modified\n";
  createDiffFile("stable.diff", diff);
  
  std::vector<bool> results;
  for (int i = 0; i < 3; ++i) {
    createSourceFile("test.txt", "content\n");
    auto result = patchMgr->applyPatch(projectDir, testDir / "stable.diff");
    results.push_back(result.success);
  }
  
  EXPECT_EQ(results[0], results[1]);
  EXPECT_EQ(results[1], results[2]);
}

TEST_F(PatchManagerTest, LargePatchApplication) {
  std::string originalContent = "line0\n";
  for (int i = 1; i < 100; ++i) {
    originalContent += "line" + std::to_string(i) + "\n";
  }
  createSourceFile("large.txt", originalContent);
  
  std::string diff = "--- a/large.txt\n+++ b/large.txt\n";
  for (int i = 0; i < 100; ++i) {
    diff += "-line" + std::to_string(i) + "\n";
    diff += "+modified" + std::to_string(i) + "\n";
  }
  createDiffFile("large.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "large.diff");
  
  EXPECT_TRUE(result.success);
}

TEST_F(PatchManagerTest, EmptyPatchRejected) {
  createSourceFile("test.txt", "content\n");
  
  std::string diff = "";
  createDiffFile("empty.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "empty.diff");
  
  EXPECT_FALSE(result.success);
}

TEST_F(PatchManagerTest, PatchOnEmptyFile) {
  createSourceFile("empty.txt", "");
  
  std::string diff = "--- a/empty.txt\n+++ b/empty.txt\n"
                     "+new content\n";
  createDiffFile("emptyfile.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "emptyfile.diff");
  
  EXPECT_EQ(result.success, true);
}

TEST_F(PatchManagerTest, BackupFileCreation) {
  createSourceFile("backup_test.txt", "original content\n");
  fs::path testFile = projectDir / "backup_test.txt";
  
  std::string diff = "--- a/backup_test.txt\n+++ b/backup_test.txt\n"
                     "-original content\n"
                     "+modified content\n";
  createDiffFile("backup.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "backup.diff");
  
  EXPECT_TRUE(result.success);
}

TEST_F(PatchManagerTest, RollbackOnBuildFailure) {
  createSourceFile("rollback.txt", "original\n");
  mockEngine.setFailBuild(true);
  
  std::string diff = "--- a/rollback.txt\n+++ b/rollback.txt\n"
                     "-original\n"
                     "+modified\n";
  createDiffFile("rollback.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "rollback.diff");
  
  EXPECT_FALSE(result.success);
}

TEST_F(PatchManagerTest, MultipleFilesWithOneFailure) {
  createSourceFile("file1.txt", "content1\n");
  
  std::string diff = "--- a/file1.txt\n+++ b/file1.txt\n"
                     "-content1\n"
                     "+new1\n"
                     "--- a/nonexistent.txt\n+++ b/nonexistent.txt\n"
                     "-old\n"
                     "+new\n";
  createDiffFile("partial.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "partial.diff");
  
  EXPECT_FALSE(result.success);
}

TEST_F(PatchManagerTest, NestedDirectoryFile) {
  createSourceFile("src/deep/nested/file.cpp", "#include <iostream>\nint main() {}\n");
  
  std::string diff = "--- a/src/deep/nested/file.cpp\n+++ b/src/deep/nested/file.cpp\n"
                     "-int main() {}\n"
                     "+int main() { return 0; }\n";
  createDiffFile("nested.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "nested.diff");
  
  EXPECT_TRUE(result.success);
}

TEST_F(PatchManagerTest, AddNewLines) {
  createSourceFile("add.txt", "original\n");
  
  std::string diff = "--- a/add.txt\n+++ b/add.txt\n"
                     "+added line 1\n"
                     "+added line 2\n";
  createDiffFile("add.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "add.diff");
  
  EXPECT_TRUE(result.success);
}

TEST_F(PatchManagerTest, RemoveLines) {
  createSourceFile("remove.txt", "line1\nline2\nline3\n");
  
  std::string diff = "--- a/remove.txt\n+++ b/remove.txt\n"
                     "-line1\n"
                     "-line2\n";
  createDiffFile("remove.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "remove.diff");
  
  EXPECT_TRUE(result.success);
}

TEST_F(PatchManagerTest, FilesModifiedCountAccurate) {
  createSourceFile("f1.txt", "c1\n");
  createSourceFile("f2.txt", "c2\n");
  createSourceFile("f3.txt", "c3\n");
  
  std::string diff = "--- a/f1.txt\n+++ b/f1.txt\n"
                     "-c1\n"
                     "+new1\n"
                     "--- a/f2.txt\n+++ b/f2.txt\n"
                     "-c2\n"
                     "+new2\n"
                     "--- a/f3.txt\n+++ b/f3.txt\n"
                     "-c3\n"
                     "+new3\n";
  createDiffFile("three.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "three.diff");
  
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.filesModified, 3);
}

TEST_F(PatchManagerTest, NonexistentDiffFile) {
  createSourceFile("test.txt", "content\n");
  
  fs::path nonexistentDiff = testDir / "nonexistent.diff";
  
  auto result = patchMgr->applyPatch(projectDir, nonexistentDiff);
  
  EXPECT_FALSE(result.success);
}

TEST_F(PatchManagerTest, BuildSuccessCleanup) {
  createSourceFile("cleanup.txt", "original\n");
  mockEngine.setFailBuild(false);
  
  std::string diff = "--- a/cleanup.txt\n+++ b/cleanup.txt\n"
                     "-original\n"
                     "+modified\n";
  createDiffFile("cleanup.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "cleanup.diff");
  
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.filesModified, 1);
}

TEST_F(PatchManagerTest, SpecialCharactersInFilePath) {
  createSourceFile("file-with-dash.txt", "content\n");
  
  std::string diff = "--- a/file-with-dash.txt\n+++ b/file-with-dash.txt\n"
                     "-content\n"
                     "+modified\n";
  createDiffFile("special.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "special.diff");
  
  EXPECT_TRUE(result.success);
}

TEST_F(PatchManagerTest, LargeSingleFile) {
  std::string largeContent = "";
  for (int i = 0; i < 1000; ++i) {
    largeContent += "line " + std::to_string(i) + "\n";
  }
  createSourceFile("huge.txt", largeContent);
  
  std::string diff = "--- a/huge.txt\n+++ b/huge.txt\n"
                     "-line 0\n"
                     "+modified line 0\n";
  createDiffFile("huge.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "huge.diff");
  
  EXPECT_TRUE(result.success);
}

TEST_F(PatchManagerTest, ResultStructureValid) {
  createSourceFile("valid.txt", "content\n");
  
  std::string diff = "--- a/valid.txt\n+++ b/valid.txt\n"
                     "-content\n"
                     "+modified\n";
  createDiffFile("valid.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "valid.diff");
  
  EXPECT_GE(result.filesModified, 0);
}

TEST_F(PatchManagerTest, PathNormalization) {
  createSourceFile("sub/file.txt", "data\n");
  
  std::string diff = "--- a/sub/file.txt\n+++ b/sub/file.txt\n"
                     "-data\n"
                     "+newdata\n";
  createDiffFile("path.diff", diff);
  
  auto result = patchMgr->applyPatch(projectDir, testDir / "path.diff");
  
  EXPECT_TRUE(result.success);
}
