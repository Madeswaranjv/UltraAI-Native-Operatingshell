// ============================================================================
// File: tests/incremental/test_integrity_manager.cpp
// Tests for IntegrityManager schema and index verification
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include "ai/IntegrityManager.h"
#include "ai/FileRegistry.h"

using namespace ultra::ai;
namespace fs = std::filesystem;

class IntegrityManagerTest : public ::testing::Test {
 protected:
  fs::path testDir;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "integrity";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
  }

  void TearDown() override {
    fs::remove_all(testDir);
  }

  void createTestFile(const fs::path& path, const std::string& content = "test") {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << content;
  }
};

TEST_F(IntegrityManagerTest, BuildCoreIndexWithMagic) {
  auto hash = zeroHash();
  auto core = IntegrityManager::buildCoreIndex(true, hash, hash, hash, hash, hash);
  
  EXPECT_EQ(core.magic, IntegrityManager::kCoreMagic);
}

TEST_F(IntegrityManagerTest, BuildCoreIndexVersions) {
  auto hash = zeroHash();
  auto core = IntegrityManager::buildCoreIndex(true, hash, hash, hash, hash, hash);
  
  EXPECT_EQ(core.indexVersion, IntegrityManager::kIndexVersion);
  EXPECT_EQ(core.schemaVersion, IntegrityManager::kSchemaVersion);
}

TEST_F(IntegrityManagerTest, BuildCoreIndexRuntimeActiveTrue) {
  auto hash = zeroHash();
  auto core = IntegrityManager::buildCoreIndex(true, hash, hash, hash, hash, hash);
  
  EXPECT_EQ(core.runtimeActive, 1U);
}

TEST_F(IntegrityManagerTest, BuildCoreIndexRuntimeActiveFalse) {
  auto hash = zeroHash();
  auto core = IntegrityManager::buildCoreIndex(false, hash, hash, hash, hash, hash);
  
  EXPECT_EQ(core.runtimeActive, 0U);
}

TEST_F(IntegrityManagerTest, BuildCoreIndexHashesPreserved) {
  auto h1 = sha256OfString("files");
  auto h2 = sha256OfString("symbols");
  auto h3 = sha256OfString("deps");
  auto h4 = sha256OfString("root");
  auto h5 = sha256OfString("index");
  
  auto core = IntegrityManager::buildCoreIndex(true, h1, h2, h3, h4, h5);
  
  EXPECT_TRUE(hashesEqual(core.filesTblHash, h1));
  EXPECT_TRUE(hashesEqual(core.symbolsTblHash, h2));
  EXPECT_TRUE(hashesEqual(core.depsTblHash, h3));
  EXPECT_TRUE(hashesEqual(core.projectRootHash, h4));
  EXPECT_TRUE(hashesEqual(core.indexHash, h5));
}

TEST_F(IntegrityManagerTest, ComputeProjectRootHashEmpty) {
  std::vector<FileRecord> files;
  
  auto hash = IntegrityManager::computeProjectRootHash(files);
  
  EXPECT_EQ(hash.size(), 32);
}

TEST_F(IntegrityManagerTest, ComputeProjectRootHashSingleFile) {
  std::vector<FileRecord> files;
  FileRecord rec;
  rec.path = "file.cpp";
  rec.hash = sha256OfString("content");
  files.push_back(rec);
  
  auto hash = IntegrityManager::computeProjectRootHash(files);
  
  EXPECT_EQ(hash.size(), 32);
}

TEST_F(IntegrityManagerTest, ComputeProjectRootHashMultipleFiles) {
  std::vector<FileRecord> files;
  for (int i = 0; i < 5; ++i) {
    FileRecord rec;
    rec.path = "file" + std::to_string(i) + ".cpp";
    rec.hash = sha256OfString("content" + std::to_string(i));
    files.push_back(rec);
  }
  
  auto hash = IntegrityManager::computeProjectRootHash(files);
  
  EXPECT_EQ(hash.size(), 32);
}

TEST_F(IntegrityManagerTest, ComputeProjectRootHashDeterministic) {
  std::vector<FileRecord> files;
  files.push_back(FileRecord{"file1.cpp", sha256OfString("c1")});
  files.push_back(FileRecord{"file2.cpp", sha256OfString("c2")});
  
  auto hash1 = IntegrityManager::computeProjectRootHash(files);
  auto hash2 = IntegrityManager::computeProjectRootHash(files);
  
  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

TEST_F(IntegrityManagerTest, ComputeTableHashFromFile) {
  fs::path tablePath = testDir / "table.db";
  createTestFile(tablePath, "table data");
  
  Sha256Hash hash;
  std::string error;
  bool success = IntegrityManager::computeTableHash(tablePath, hash, error);
  
  EXPECT_TRUE(success);
  EXPECT_EQ(hash.size(), 32);
}

TEST_F(IntegrityManagerTest, ComputeTableHashNonexistentFile) {
  fs::path tablePath = testDir / "nonexistent.db";
  
  Sha256Hash hash;
  std::string error;
  bool success = IntegrityManager::computeTableHash(tablePath, hash, error);
  
  EXPECT_FALSE(success);
}

TEST_F(IntegrityManagerTest, ComputeTableHashDeterministic) {
  fs::path tablePath = testDir / "stable.db";
  createTestFile(tablePath, "stable content");
  
  Sha256Hash hash1;
  Sha256Hash hash2;
  std::string error;
  
  IntegrityManager::computeTableHash(tablePath, hash1, error);
  IntegrityManager::computeTableHash(tablePath, hash2, error);
  
  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

TEST_F(IntegrityManagerTest, ComputeIndexHashThreeFiles) {
  fs::path filesTable = testDir / "files.tbl";
  fs::path symbolsTable = testDir / "symbols.tbl";
  fs::path depsTable = testDir / "deps.tbl";
  
  createTestFile(filesTable, "files");
  createTestFile(symbolsTable, "symbols");
  createTestFile(depsTable, "deps");
  
  Sha256Hash hash;
  std::string error;
  bool success = IntegrityManager::computeIndexHash(filesTable, symbolsTable, depsTable, hash, error);
  
  EXPECT_TRUE(success);
  EXPECT_EQ(hash.size(), 32);
}

TEST_F(IntegrityManagerTest, ComputeIndexHashMissingFile) {
  fs::path filesTable = testDir / "files.tbl";
  fs::path symbolsTable = testDir / "nonexistent.tbl";
  fs::path depsTable = testDir / "deps.tbl";
  
  createTestFile(filesTable, "files");
  createTestFile(depsTable, "deps");
  
  Sha256Hash hash;
  std::string error;
  bool success = IntegrityManager::computeIndexHash(filesTable, symbolsTable, depsTable, hash, error);
  
  EXPECT_FALSE(success);
}

TEST_F(IntegrityManagerTest, ComputeIndexHashDeterministic) {
  fs::path filesTable = testDir / "f.tbl";
  fs::path symbolsTable = testDir / "s.tbl";
  fs::path depsTable = testDir / "d.tbl";
  
  createTestFile(filesTable, "f");
  createTestFile(symbolsTable, "s");
  createTestFile(depsTable, "d");
  
  Sha256Hash hash1, hash2;
  std::string error;
  
  IntegrityManager::computeIndexHash(filesTable, symbolsTable, depsTable, hash1, error);
  IntegrityManager::computeIndexHash(filesTable, symbolsTable, depsTable, hash2, error);
  
  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

TEST_F(IntegrityManagerTest, VerifySuccessScenario) {
  auto h1 = sha256OfString("f");
  auto h2 = sha256OfString("s");
  auto h3 = sha256OfString("d");
  auto h4 = sha256OfString("r");
  auto h5 = sha256OfString("i");
  
  auto core = IntegrityManager::buildCoreIndex(true, h1, h2, h3, h4, h5);
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, h2, h3, h4, h5, error);
  
  EXPECT_TRUE(success);
}

TEST_F(IntegrityManagerTest, VerifyMagicMismatch) {
  auto h1 = sha256OfString("f");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h1, h1, h1, h1);
  core.magic = 0xDEADBEEF;
  
  auto wrongH = sha256OfString("wrong");
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, h1, h1, h1, h1, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "core.idx magic mismatch");
}

TEST_F(IntegrityManagerTest, VerifyIndexVersionMismatch) {
  auto h1 = sha256OfString("f");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h1, h1, h1, h1);
  core.indexVersion = 999U;
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, h1, h1, h1, h1, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "core.idx index_version mismatch");
}

TEST_F(IntegrityManagerTest, VerifySchemaVersionMismatch) {
  auto h1 = sha256OfString("f");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h1, h1, h1, h1);
  core.schemaVersion = 999U;
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, h1, h1, h1, h1, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "core.idx schema_version mismatch");
}

TEST_F(IntegrityManagerTest, VerifyFilesTblHashMismatch) {
  auto h1 = sha256OfString("f");
  auto h2 = sha256OfString("s");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h2, h2, h2, h2);
  
  auto wrongH = sha256OfString("wrong");
  
  std::string error;
  bool success = IntegrityManager::verify(core, wrongH, h2, h2, h2, h2, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "files.tbl hash mismatch");
}

TEST_F(IntegrityManagerTest, VerifySymbolsTblHashMismatch) {
  auto h1 = sha256OfString("f");
  auto h2 = sha256OfString("s");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h2, h2, h2, h2);
  
  auto wrongH = sha256OfString("wrong");
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, wrongH, h2, h2, h2, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "symbols.tbl hash mismatch");
}

TEST_F(IntegrityManagerTest, VerifyDepsTblHashMismatch) {
  auto h1 = sha256OfString("f");
  auto h2 = sha256OfString("s");
  auto h3 = sha256OfString("d");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h2, h3, h3, h3);
  
  auto wrongH = sha256OfString("wrong");
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, h2, wrongH, h3, h3, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "deps.tbl hash mismatch");
}

TEST_F(IntegrityManagerTest, VerifyProjectRootHashMismatch) {
  auto h1 = sha256OfString("f");
  auto h2 = sha256OfString("s");
  auto h3 = sha256OfString("d");
  auto h4 = sha256OfString("r");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h2, h3, h4, h4);
  
  auto wrongH = sha256OfString("wrong");
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, h2, h3, wrongH, h4, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "project_root_hash mismatch");
}

TEST_F(IntegrityManagerTest, VerifyIndexHashMismatch) {
  auto h1 = sha256OfString("f");
  auto h2 = sha256OfString("s");
  auto h3 = sha256OfString("d");
  auto h4 = sha256OfString("r");
  auto h5 = sha256OfString("i");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h2, h3, h4, h5);
  
  auto wrongH = sha256OfString("wrong");
  
  std::string error;
  bool success = IntegrityManager::verify(core, h1, h2, h3, h4, wrongH, error);
  
  EXPECT_FALSE(success);
  EXPECT_EQ(error, "index_hash mismatch");
}

TEST_F(IntegrityManagerTest, VerifyMultipleMismatches) {
  auto h1 = sha256OfString("f");
  auto core = IntegrityManager::buildCoreIndex(true, h1, h1, h1, h1, h1);
  core.magic = 0xBAD;
  core.indexVersion = 999U;
  
  auto wrongH = sha256OfString("wrong");
  
  std::string error;
  bool success = IntegrityManager::verify(core, wrongH, h1, h1, h1, h1, error);
  
  EXPECT_FALSE(success);
}

TEST_F(IntegrityManagerTest, CoreIndexStructSize) {
  auto hash = zeroHash();
  auto core = IntegrityManager::buildCoreIndex(true, hash, hash, hash, hash, hash);
  
  EXPECT_GE(sizeof(core), 64);
}

TEST_F(IntegrityManagerTest, DeterministicBuild) {
  auto h1 = sha256OfString("data");
  
  auto core1 = IntegrityManager::buildCoreIndex(true, h1, h1, h1, h1, h1);
  auto core2 = IntegrityManager::buildCoreIndex(true, h1, h1, h1, h1, h1);
  
  EXPECT_EQ(core1.magic, core2.magic);
  EXPECT_EQ(core1.indexVersion, core2.indexVersion);
  EXPECT_TRUE(hashesEqual(core1.filesTblHash, core2.filesTblHash));
}

TEST_F(IntegrityManagerTest, ReservedBytesZero) {
  auto hash = zeroHash();
  auto core = IntegrityManager::buildCoreIndex(true, hash, hash, hash, hash, hash);
  
  EXPECT_EQ(core.reserved[0], 0U);
  EXPECT_EQ(core.reserved[1], 0U);
  EXPECT_EQ(core.reserved[2], 0U);
}
