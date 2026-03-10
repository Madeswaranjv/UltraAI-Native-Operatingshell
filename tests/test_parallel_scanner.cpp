#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ai/Hashing.h"
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

std::vector<std::string> canonicalFiles(const ultra::engine::ScanOutput& output) {
  std::vector<std::string> rows;
  rows.reserve(output.files.size());
  for (const auto& file : output.files) {
    rows.push_back(file.path + "|" + std::to_string(file.fileId) + "|" +
                   std::to_string(static_cast<int>(file.language)) + "|" +
                   std::to_string(file.lastModified) + "|" +
                   ultra::ai::hashToHex(file.hash));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSymbols(const ultra::engine::ScanOutput& output) {
  std::vector<std::string> rows;
  rows.reserve(output.symbols.size());
  for (const auto& symbol : output.symbols) {
    rows.push_back(std::to_string(symbol.symbolId) + "|" +
                   std::to_string(symbol.fileId) + "|" + symbol.name + "|" +
                   symbol.signature + "|" +
                   std::to_string(static_cast<int>(symbol.symbolType)) + "|" +
                   std::to_string(static_cast<int>(symbol.visibility)) + "|" +
                   std::to_string(symbol.lineNumber));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalFileEdges(const ultra::engine::ScanOutput& output) {
  std::vector<std::string> rows;
  rows.reserve(output.deps.fileEdges.size());
  for (const auto& edge : output.deps.fileEdges) {
    rows.push_back(std::to_string(edge.fromFileId) + "->" +
                   std::to_string(edge.toFileId));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSymbolEdges(
    const ultra::engine::ScanOutput& output) {
  std::vector<std::string> rows;
  rows.reserve(output.deps.symbolEdges.size());
  for (const auto& edge : output.deps.symbolEdges) {
    rows.push_back(std::to_string(edge.fromSymbolId) + "->" +
                   std::to_string(edge.toSymbolId));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSemanticDeps(
    const ultra::engine::ScanOutput& output) {
  std::vector<std::string> rows;
  for (const auto& [fileId, deps] : output.semanticSymbolDepsByFileId) {
    for (const auto& dep : deps) {
      rows.push_back(std::to_string(fileId) + "|" + dep.fromSymbol + "->" +
                     dep.toSymbol + "|" + std::to_string(dep.lineNumber));
    }
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSymbolIndex(
    const ultra::engine::ScanOutput& output) {
  std::vector<std::string> rows;
  rows.reserve(output.symbolIndex.size());
  for (const auto& [name, node] : output.symbolIndex) {
    std::vector<std::string> used(node.usedInFiles.begin(), node.usedInFiles.end());
    std::sort(used.begin(), used.end());
    std::string usedConcat;
    for (const std::string& path : used) {
      usedConcat += path + ";";
    }
    rows.push_back(name + "|" + node.definedIn + "|" + usedConcat + "|" +
                   std::to_string(node.centrality) + "|" +
                   std::to_string(node.weight));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

}  // namespace

class ParallelScannerTest : public ::testing::Test {
 protected:
  fs::path testRoot;

  void SetUp() override {
    testRoot = fs::temp_directory_path() / "ultra_test" / "parallel_scanner";
    fs::remove_all(testRoot);
    fs::create_directories(testRoot);
  }

  void TearDown() override {
    fs::remove_all(testRoot);
  }

  void writeProject() {
    writeTextFile(testRoot / "lib.h", "int helper();\n");
    writeTextFile(testRoot / "lib.cpp",
                  "#include \"lib.h\"\n"
                  "int helper() {\n"
                  "  return 7;\n"
                  "}\n");
    writeTextFile(testRoot / "app.cpp",
                  "#include \"lib.h\"\n"
                  "int run() {\n"
                  "  return helper();\n"
                  "}\n");
    writeTextFile(testRoot / "util.py",
                  "def make_value(x):\n"
                  "    return x + 1\n");
  }
};

TEST_F(ParallelScannerTest, FullScanParallelDeterministicAcrossRuns) {
  writeProject();

  ultra::engine::Scanner scanner(testRoot);
  ultra::engine::ScanOutput first;
  ultra::engine::ScanOutput second;
  std::string error;

  ASSERT_TRUE(scanner.fullScanParallel(first, error)) << error;
  ASSERT_TRUE(scanner.fullScanParallel(second, error)) << error;

  EXPECT_EQ(canonicalFiles(first), canonicalFiles(second));
  EXPECT_EQ(canonicalSymbols(first), canonicalSymbols(second));
  EXPECT_EQ(canonicalFileEdges(first), canonicalFileEdges(second));
  EXPECT_EQ(canonicalSymbolEdges(first), canonicalSymbolEdges(second));
  EXPECT_EQ(canonicalSemanticDeps(first), canonicalSemanticDeps(second));
  EXPECT_EQ(canonicalSymbolIndex(first), canonicalSymbolIndex(second));
}
