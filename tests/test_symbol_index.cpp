#include <gtest/gtest.h>
//E:\Projects\Ultra\tests\test_symbol_index.cpp
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

#include "engine/scanner.h"

namespace fs = std::filesystem;

namespace {

void writeTextFile(const fs::path& path, const std::string& content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(static_cast<bool>(out));
  out << content;
  ASSERT_TRUE(static_cast<bool>(out));
}

bool setContains(const std::unordered_set<std::string>& values,
                 const std::string& target) {
  return values.find(target) != values.end();
}

}  // namespace

class SymbolIndexTest : public ::testing::Test {
 protected:
  fs::path testRoot;

  void SetUp() override {
    testRoot = fs::temp_directory_path() / "ultra_test" / "symbol_index";
    fs::remove_all(testRoot);
    fs::create_directories(testRoot);
  }

  void TearDown() override {
    fs::remove_all(testRoot);
  }

  void writeBaselineFiles() {
    writeTextFile(testRoot / "lib.cpp",
                  "int sha256OfString() {\n"
                  "  return 7;\n"
                  "}\n");

    writeTextFile(testRoot / "user.cpp",
                  "#include \"lib.cpp\"\n"
                  "int runQuery() {\n"
                  "  return sha256OfString();\n"
                  "}\n");
  }
};

TEST_F(SymbolIndexTest, FullScanBuildsSymbolIndexWithDefinitionAndUsage) {
  writeBaselineFiles();

  ultra::engine::Scanner scanner(testRoot);
  ultra::engine::ScanOutput output;
  std::string error;

  ASSERT_TRUE(scanner.fullScanParallel(output, error)) << error;

  const auto nodeIt = output.symbolIndex.find("sha256OfString");
  ASSERT_NE(nodeIt, output.symbolIndex.end());

  EXPECT_EQ(nodeIt->second.definedIn, "lib.cpp");
  EXPECT_TRUE(setContains(nodeIt->second.usedInFiles, "user.cpp"));
  EXPECT_GT(nodeIt->second.weight, 0.0);
}

TEST_F(SymbolIndexTest, IncrementalModifyRemovesStaleSymbolAndAddsNewOne) {
  writeBaselineFiles();

  ultra::engine::Scanner scanner(testRoot);
  ultra::engine::ScanOutput initialOutput;
  std::string error;

  ASSERT_TRUE(scanner.fullScanParallel(initialOutput, error)) << error;

  ultra::ai::RuntimeState state;
  state.files = initialOutput.files;
  state.symbols = initialOutput.symbols;
  state.deps = initialOutput.deps;
  state.symbolIndex = initialOutput.symbolIndex;

  // 🔧 FIX: restore semantic dependencies
  state.semanticSymbolDepsByFileId = initialOutput.semanticSymbolDepsByFileId;

  writeTextFile(testRoot / "lib.cpp",
                "int renamedHash() {\n"
                "  return 11;\n"
                "}\n");

  ultra::engine::ScanOutput incrementalOutput;

  ASSERT_TRUE(scanner.incrementalModify(state, "lib.cpp", incrementalOutput, error))
      << error;

  EXPECT_EQ(state.symbolIndex.find("sha256OfString"), state.symbolIndex.end());

  const auto renamedIt = state.symbolIndex.find("renamedHash");
  ASSERT_NE(renamedIt, state.symbolIndex.end());

  EXPECT_EQ(renamedIt->second.definedIn, "lib.cpp");
}

TEST_F(SymbolIndexTest, IncrementalRemoveClearsRemovedFileSymbolEntries) {
  writeBaselineFiles();

  ultra::engine::Scanner scanner(testRoot);
  ultra::engine::ScanOutput initialOutput;
  std::string error;

  ASSERT_TRUE(scanner.fullScanParallel(initialOutput, error)) << error;

  ultra::ai::RuntimeState state;
  state.files = initialOutput.files;
  state.symbols = initialOutput.symbols;
  state.deps = initialOutput.deps;
  state.symbolIndex = initialOutput.symbolIndex;

  // 🔧 FIX: restore semantic dependencies
  state.semanticSymbolDepsByFileId = initialOutput.semanticSymbolDepsByFileId;

  ASSERT_TRUE(fs::remove(testRoot / "lib.cpp"));

  ultra::engine::ScanOutput incrementalOutput;

  ASSERT_TRUE(scanner.incrementalModify(state, "lib.cpp", incrementalOutput, error))
      << error;

  EXPECT_EQ(state.symbolIndex.find("sha256OfString"), state.symbolIndex.end());

  for (const auto& [name, node] : state.symbolIndex) {
    (void)name;
    EXPECT_NE(node.definedIn, "lib.cpp");
  }
}