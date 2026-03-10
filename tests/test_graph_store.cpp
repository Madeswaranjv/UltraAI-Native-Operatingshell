#include <gtest/gtest.h>

#include "ai/Hashing.h"
#include "ai/SymbolTable.h"
#include "core/graph_store/GraphLoader.h"
#include "core/graph_store/GraphStore.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

ultra::ai::RuntimeState makeState(const std::uint32_t fileCount) {
  ultra::ai::RuntimeState state;

  for (std::uint32_t index = 1U; index <= fileCount; ++index) {
    ultra::ai::FileRecord file;
    file.fileId = index;
    file.path = "src/file_" + std::to_string(index) + ".cpp";
    file.hash = ultra::ai::sha256OfString("file_" + std::to_string(index));
    file.language = ultra::ai::Language::Cpp;
    file.lastModified = 1000ULL + static_cast<std::uint64_t>(index);
    state.files.push_back(file);

    ultra::ai::SymbolRecord symbol;
    symbol.fileId = index;
    symbol.symbolId = ultra::ai::SymbolTable::composeSymbolId(index, 1U);
    symbol.name = "sym_" + std::to_string(index);
    symbol.signature = "int sym_" + std::to_string(index) + "()";
    symbol.symbolType = ultra::ai::SymbolType::Function;
    symbol.visibility = ultra::ai::Visibility::Public;
    symbol.lineNumber = 10U + index;
    state.symbols.push_back(symbol);

    if (index < fileCount) {
      state.deps.fileEdges.push_back({index, index + 1U});
      state.deps.fileEdges.push_back({index, index + 1U});  // duplicate on purpose
      state.deps.symbolEdges.push_back(
          {ultra::ai::SymbolTable::composeSymbolId(index, 1U),
           ultra::ai::SymbolTable::composeSymbolId(index + 1U, 1U)});
      state.deps.symbolEdges.push_back(
          {ultra::ai::SymbolTable::composeSymbolId(index, 1U),
           ultra::ai::SymbolTable::composeSymbolId(index + 1U, 1U)});  // duplicate

      ultra::ai::SemanticSymbolDependency dep;
      dep.fromSymbol = "sym_" + std::to_string(index);
      dep.toSymbol = "sym_" + std::to_string(index + 1U);
      dep.lineNumber = 50U + index;
      state.semanticSymbolDepsByFileId[index].push_back(dep);
      state.semanticSymbolDepsByFileId[index].push_back(dep);  // duplicate
    }
  }

  ultra::core::graph_store::GraphLoader::normalizeRuntimeState(state);
  ultra::core::graph_store::GraphLoader::rebuildSymbolIndex(state);
  return state;
}

std::vector<std::string> canonicalFiles(const ultra::ai::RuntimeState& state) {
  std::vector<std::string> rows;
  rows.reserve(state.files.size());
  for (const ultra::ai::FileRecord& file : state.files) {
    rows.push_back(file.path + "|" + std::to_string(file.fileId) + "|" +
                   std::to_string(static_cast<int>(file.language)) + "|" +
                   std::to_string(file.lastModified) + "|" +
                   ultra::ai::hashToHex(file.hash));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSymbols(const ultra::ai::RuntimeState& state) {
  std::vector<std::string> rows;
  rows.reserve(state.symbols.size());
  for (const ultra::ai::SymbolRecord& symbol : state.symbols) {
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

std::vector<std::string> canonicalFileEdges(const ultra::ai::RuntimeState& state) {
  std::vector<std::string> rows;
  rows.reserve(state.deps.fileEdges.size());
  for (const ultra::ai::FileDependencyEdge& edge : state.deps.fileEdges) {
    rows.push_back(std::to_string(edge.fromFileId) + "->" +
                   std::to_string(edge.toFileId));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSymbolEdges(
    const ultra::ai::RuntimeState& state) {
  std::vector<std::string> rows;
  rows.reserve(state.deps.symbolEdges.size());
  for (const ultra::ai::SymbolDependencyEdge& edge : state.deps.symbolEdges) {
    rows.push_back(std::to_string(edge.fromSymbolId) + "->" +
                   std::to_string(edge.toSymbolId));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSemanticDeps(
    const ultra::ai::RuntimeState& state) {
  std::vector<std::string> rows;
  for (const auto& [fileId, deps] : state.semanticSymbolDepsByFileId) {
    for (const ultra::ai::SemanticSymbolDependency& dep : deps) {
      rows.push_back(std::to_string(fileId) + "|" + dep.fromSymbol + "->" +
                     dep.toSymbol + "|" + std::to_string(dep.lineNumber));
    }
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> canonicalSymbolIndex(
    const ultra::ai::RuntimeState& state) {
  std::vector<std::string> rows;
  rows.reserve(state.symbolIndex.size());
  for (const auto& [name, node] : state.symbolIndex) {
    std::vector<std::string> used(node.usedInFiles.begin(), node.usedInFiles.end());
    std::sort(used.begin(), used.end());
    std::string usedConcat;
    for (const std::string& path : used) {
      usedConcat += path + ";";
    }
    std::ostringstream weight;
    weight << std::fixed << std::setprecision(10) << node.weight;
    std::ostringstream centrality;
    centrality << std::fixed << std::setprecision(10) << node.centrality;
    rows.push_back(name + "|" + node.definedIn + "|" + usedConcat + "|" +
                   weight.str() + "|" + centrality.str());
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

std::vector<std::string> listRelativeFiles(const fs::path& root) {
  std::vector<std::string> files;
  if (!fs::exists(root)) {
    return files;
  }
  for (const auto& entry :
       fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    files.push_back(entry.path().lexically_relative(root).generic_string());
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<std::uint8_t> readBytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
}

class GraphStoreTest : public ::testing::Test {
 protected:
  fs::path root;

  void SetUp() override {
    root = fs::temp_directory_path() / "ultra_test" / "graph_store";
    fs::remove_all(root);
    fs::create_directories(root);
  }

  void TearDown() override {
    fs::remove_all(root);
  }
};

}  // namespace

TEST_F(GraphStoreTest, GraphStorePersistsAndLoadsGraph) {
  ultra::core::graph_store::GraphStore store(root / "persist_load", 2U);
  const ultra::ai::RuntimeState expected = makeState(5U);

  std::string error;
  ASSERT_TRUE(store.persistFull(expected, error)) << error;

  ultra::ai::RuntimeState loaded;
  ASSERT_TRUE(store.load(loaded, error)) << error;

  EXPECT_EQ(canonicalFiles(expected), canonicalFiles(loaded));
  EXPECT_EQ(canonicalSymbols(expected), canonicalSymbols(loaded));
  EXPECT_EQ(canonicalFileEdges(expected), canonicalFileEdges(loaded));
  EXPECT_EQ(canonicalSymbolEdges(expected), canonicalSymbolEdges(loaded));
  EXPECT_EQ(canonicalSemanticDeps(expected), canonicalSemanticDeps(loaded));
  EXPECT_EQ(canonicalSymbolIndex(expected), canonicalSymbolIndex(loaded));
}

TEST_F(GraphStoreTest, GraphStoreHandlesIncrementalUpdates) {
  ultra::core::graph_store::GraphStore store(root / "incremental", 2U);
  std::string error;

  const ultra::ai::RuntimeState initial = makeState(5U);
  ASSERT_TRUE(store.persistFull(initial, error)) << error;

  ultra::ai::RuntimeState updated = makeState(6U);
  for (ultra::ai::FileRecord& file : updated.files) {
    if (file.fileId == 3U) {
      file.hash = ultra::ai::sha256OfString("file_3_changed");
      file.lastModified += 77U;
      break;
    }
  }
  ultra::core::graph_store::GraphLoader::normalizeRuntimeState(updated);
  ultra::core::graph_store::GraphLoader::rebuildSymbolIndex(updated);

  ASSERT_TRUE(store.applyIncremental(updated, {3U, 6U}, error)) << error;

  ultra::ai::RuntimeState loaded;
  ASSERT_TRUE(store.load(loaded, error)) << error;

  EXPECT_EQ(canonicalFiles(updated), canonicalFiles(loaded));
  EXPECT_EQ(canonicalSymbols(updated), canonicalSymbols(loaded));
  EXPECT_EQ(canonicalFileEdges(updated), canonicalFileEdges(loaded));
  EXPECT_EQ(canonicalSymbolEdges(updated), canonicalSymbolEdges(loaded));
  EXPECT_EQ(canonicalSemanticDeps(updated), canonicalSemanticDeps(loaded));
  EXPECT_EQ(canonicalSymbolIndex(updated), canonicalSymbolIndex(loaded));
}

TEST_F(GraphStoreTest, GraphStoreMaintainsDeterministicOrdering) {
  ultra::ai::RuntimeState first = makeState(6U);
  ultra::ai::RuntimeState second = first;

  std::reverse(second.files.begin(), second.files.end());
  std::reverse(second.symbols.begin(), second.symbols.end());
  std::reverse(second.deps.fileEdges.begin(), second.deps.fileEdges.end());
  std::reverse(second.deps.symbolEdges.begin(), second.deps.symbolEdges.end());
  for (auto& [fileId, deps] : second.semanticSymbolDepsByFileId) {
    (void)fileId;
    std::reverse(deps.begin(), deps.end());
  }
  ultra::core::graph_store::GraphLoader::normalizeRuntimeState(second);
  ultra::core::graph_store::GraphLoader::rebuildSymbolIndex(second);

  ultra::core::graph_store::GraphStore storeA(root / "deterministic_a", 2U);
  ultra::core::graph_store::GraphStore storeB(root / "deterministic_b", 2U);
  std::string error;
  ASSERT_TRUE(storeA.persistFull(first, error)) << error;
  ASSERT_TRUE(storeB.persistFull(second, error)) << error;

  const std::vector<std::string> filesA = listRelativeFiles(root / "deterministic_a");
  const std::vector<std::string> filesB = listRelativeFiles(root / "deterministic_b");
  ASSERT_EQ(filesA, filesB);

  for (const std::string& relative : filesA) {
    const fs::path pathA = root / "deterministic_a" / relative;
    const fs::path pathB = root / "deterministic_b" / relative;
    EXPECT_EQ(readBytes(pathA), readBytes(pathB)) << relative;
  }
}

TEST_F(GraphStoreTest, GraphStoreLoadsPartialChunks) {
  ultra::core::graph_store::GraphStore store(root / "partial_load", 2U);
  const ultra::ai::RuntimeState full = makeState(6U);
  std::string error;
  ASSERT_TRUE(store.persistFull(full, error)) << error;

  ultra::ai::RuntimeState partial;
  ASSERT_TRUE(store.loadPartial(2U, partial, error)) << error;

  EXPECT_EQ(partial.files.size(), 4U);
  EXPECT_EQ(partial.symbols.size(), 4U);
  EXPECT_TRUE(partial.symbolIndex.find("sym_1") != partial.symbolIndex.end());
  EXPECT_TRUE(partial.symbolIndex.find("sym_4") != partial.symbolIndex.end());
  EXPECT_TRUE(partial.symbolIndex.find("sym_6") == partial.symbolIndex.end());
}

