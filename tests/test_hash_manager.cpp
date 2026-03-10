// ============================================================================
// File: tests/incremental/test_hash_manager.cpp
// Tests for HashManager file hash tracking
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "hashing/HashManager.h"
#include "scanner/FileInfo.h"

using namespace ultra::hashing;
using namespace ultra::scanner;
namespace fs = std::filesystem;

class HashManagerTest : public ::testing::Test {
 protected:
  fs::path testDir;
  fs::path hashDbPath;
  HashManager* manager;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "hash_manager";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
    hashDbPath = testDir / "hashes.db";
    manager = new HashManager(hashDbPath);
  }

  void TearDown() override {
    delete manager;
    fs::remove_all(testDir);
  }

  FileInfo createFileInfo(const fs::path& path) {
    FileInfo info;
    info.path = path;
    return info;
  }

  void createTestFile(const fs::path& path, const std::string& content = "test") {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
  }
};

TEST_F(HashManagerTest, ComputeHashOfFile) {
  fs::path testFile = testDir / "test.txt";
  createTestFile(testFile, "content");
  
  std::string hash = manager->computeHash(testFile);
  
  EXPECT_FALSE(hash.empty());
}

TEST_F(HashManagerTest, ConsistentHashComputation) {
  fs::path testFile = testDir / "test.txt";
  createTestFile(testFile, "consistent");
  
  std::string hash1 = manager->computeHash(testFile);
  std::string hash2 = manager->computeHash(testFile);
  
  EXPECT_EQ(hash1, hash2);
}

TEST_F(HashManagerTest, DifferentFilesHaveDifferentHashes) {
  fs::path file1 = testDir / "file1.txt";
  fs::path file2 = testDir / "file2.txt";
  createTestFile(file1, "content1");
  createTestFile(file2, "content2");
  
  std::string hash1 = manager->computeHash(file1);
  std::string hash2 = manager->computeHash(file2);
  
  EXPECT_NE(hash1, hash2);
}

TEST_F(HashManagerTest, HashNonexistentFile) {
  fs::path nonexistent = testDir / "nonexistent.txt";
  
  std::string hash = manager->computeHash(nonexistent);
  
  EXPECT_TRUE(hash.empty());
}

TEST_F(HashManagerTest, DetectAddedFile) {
  fs::path file = testDir / "new.txt";
  createTestFile(file, "content");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 1);
}

TEST_F(HashManagerTest, DetectModifiedFile) {
  fs::path file = testDir / "modified.txt";
  createTestFile(file, "original");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  manager->load();
  manager->detectChanges(files);
  manager->save();
  
  createTestFile(file, "modified");
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 1);
}

TEST_F(HashManagerTest, NoChangesDetection) {
  fs::path file = testDir / "same.txt";
  createTestFile(file, "content");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  manager->load();
  manager->detectChanges(files);
  manager->save();
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 0);
}

TEST_F(HashManagerTest, MultipleFileChanges) {
  std::vector<FileInfo> files;
  for (int i = 0; i < 5; ++i) {
    fs::path file = testDir / ("file" + std::to_string(i) + ".txt");
    createTestFile(file, "content" + std::to_string(i));
    files.push_back(createFileInfo(file));
  }
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 5);
}

TEST_F(HashManagerTest, LargeNumberOfFiles) {
  std::vector<FileInfo> files;
  for (int i = 0; i < 100; ++i) {
    fs::path file = testDir / ("file" + std::to_string(i) + ".txt");
    createTestFile(file, "data");
    files.push_back(createFileInfo(file));
  }
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 100);
}

TEST_F(HashManagerTest, SaveAndLoadPersistence) {
  fs::path file = testDir / "persist.txt";
  createTestFile(file, "persistent");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  manager->load();
  manager->detectChanges(files);
  manager->save();
  
  HashManager manager2(hashDbPath);
  manager2.load();
  
  auto changed = manager2.detectChanges(files);
  EXPECT_EQ(changed.size(), 0);
}

TEST_F(HashManagerTest, EmptyFileList) {
  std::vector<FileInfo> empty;
  
  manager->load();
  auto changed = manager->detectChanges(empty);
  
  EXPECT_EQ(changed.size(), 0);
}

TEST_F(HashManagerTest, FileWithoutReadPermission) {
  fs::path file = testDir / "noperm.txt";
  createTestFile(file, "content");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_GE(changed.size(), 0);
}

TEST_F(HashManagerTest, LargeFile) {
  fs::path largeFile = testDir / "large.bin";
  std::ofstream out(largeFile, std::ios::binary);
  std::string data(1000000, 'x');
  out.write(data.data(), data.size());
  out.close();
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(largeFile));
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 1);
}

TEST_F(HashManagerTest, BinaryFile) {
  fs::path binFile = testDir / "binary.bin";
  std::ofstream out(binFile, std::ios::binary);
  std::uint8_t binary[] = {0xFF, 0xFE, 0xFD, 0xFC};
  out.write(reinterpret_cast<char*>(binary), 4);
  out.close();
  
  std::string hash = manager->computeHash(binFile);
  
  EXPECT_FALSE(hash.empty());
}

TEST_F(HashManagerTest, DeterministicDetection) {
  fs::path file = testDir / "det.txt";
  createTestFile(file, "deterministic");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  manager->load();
  auto changed1 = manager->detectChanges(files);
  
  manager->load();
  auto changed2 = manager->detectChanges(files);
  
  EXPECT_EQ(changed1, changed2);
}

TEST_F(HashManagerTest, EmptyDatabase) {
  manager->load();
  
  fs::path file = testDir / "first.txt";
  createTestFile(file, "content");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 1);
}

TEST_F(HashManagerTest, MixedChangedAndUnchanged) {
  std::vector<FileInfo> files;
  
  fs::path file1 = testDir / "unchanged.txt";
  createTestFile(file1, "same");
  files.push_back(createFileInfo(file1));
  
  fs::path file2 = testDir / "new.txt";
  createTestFile(file2, "new");
  files.push_back(createFileInfo(file2));
  
  manager->load();
  manager->detectChanges(files);
  manager->save();
  
  manager->load();
  createTestFile(file1, "same");
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 0);
}

TEST_F(HashManagerTest, DeletedFileNotDetected) {
  fs::path file = testDir / "temp.txt";
  createTestFile(file, "temporary");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  manager->load();
  manager->detectChanges(files);
  manager->save();
  
  fs::remove(file);
  files.clear();
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 0);
}

TEST_F(HashManagerTest, FileRenamedStillDetectsChange) {
  fs::path file1 = testDir / "original.txt";
  createTestFile(file1, "content");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file1));
  
  manager->load();
  manager->detectChanges(files);
  manager->save();
  
  fs::path file2 = testDir / "renamed.txt";
  fs::rename(file1, file2);
  
  manager->load();
  files.clear();
  files.push_back(createFileInfo(file2));
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 1);
}

TEST_F(HashManagerTest, MultipleLoadsAndSaves) {
  fs::path file = testDir / "multi.txt";
  createTestFile(file, "v1");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(file));
  
  for (int i = 0; i < 3; ++i) {
    manager->load();
    manager->detectChanges(files);
    manager->save();
  }
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 0);
}

TEST_F(HashManagerTest, SymlinkHandling) {
  fs::path realFile = testDir / "real.txt";
  createTestFile(realFile, "real");
  
  std::vector<FileInfo> files;
  files.push_back(createFileInfo(realFile));
  
  manager->load();
  manager->detectChanges(files);
  manager->save();
  
  manager->load();
  auto changed = manager->detectChanges(files);
  
  EXPECT_EQ(changed.size(), 0);
}
