#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "core/state_manager.h"
#include "engine/query/SymbolQueryEngine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

ultra::ai::SymbolRecord makeSymbol(const std::uint32_t fileId,
                                   const std::uint32_t localIndex,
                                   const std::string& name,
                                   const std::string& signature,
                                   const ultra::ai::SymbolType symbolType,
                                   const std::uint32_t lineNumber) {
  ultra::ai::SymbolRecord symbol;
  symbol.fileId = fileId;
  symbol.symbolId = ultra::ai::SymbolTable::composeSymbolId(fileId, localIndex);
  symbol.name = name;
  symbol.signature = signature;
  symbol.symbolType = symbolType;
  symbol.visibility = ultra::ai::Visibility::Public;
  symbol.lineNumber = lineNumber;
  return symbol;
}

ultra::ai::RuntimeState makeQueryState(const bool shuffled) {
  ultra::ai::RuntimeState state;

  ultra::ai::FileRecord core;
  core.fileId = 1U;
  core.path = "core.cpp";
  ultra::ai::FileRecord service;
  service.fileId = 2U;
  service.path = "service.cpp";
  ultra::ai::FileRecord app;
  app.fileId = 3U;
  app.path = "app.cpp";
  ultra::ai::FileRecord worker;
  worker.fileId = 4U;
  worker.path = "worker.cpp";
  state.files = {core, service, app, worker};

  state.symbols = {
      makeSymbol(1U, 1U, "coreFn", "int coreFn()", ultra::ai::SymbolType::Function,
                 10U),
      makeSymbol(2U, 1U, "serviceFn", "int serviceFn()", ultra::ai::SymbolType::Function,
                 20U),
      makeSymbol(3U, 1U, "appMain", "int appMain()", ultra::ai::SymbolType::Function,
                 30U),
      makeSymbol(4U, 1U, "workerTask", "int workerTask()",
                 ultra::ai::SymbolType::Function, 40U),
      makeSymbol(2U, 2U, "coreFn", "coreFn()", ultra::ai::SymbolType::Import, 21U),
      makeSymbol(3U, 2U, "serviceFn", "serviceFn()", ultra::ai::SymbolType::Import,
                 31U),
      makeSymbol(4U, 2U, "serviceFn", "serviceFn()", ultra::ai::SymbolType::Import,
                 41U),
  };

  state.deps.fileEdges = {
      {3U, 2U},
      {2U, 1U},
      {4U, 2U},
      {3U, 2U},  // duplicate by design
  };
  state.deps.symbolEdges = {
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(3U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(4U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},  // duplicate by design
  };

  if (shuffled) {
    std::reverse(state.files.begin(), state.files.end());
    std::reverse(state.symbols.begin(), state.symbols.end());
    std::reverse(state.deps.fileEdges.begin(), state.deps.fileEdges.end());
    std::reverse(state.deps.symbolEdges.begin(), state.deps.symbolEdges.end());
  }

  return state;
}

struct ScopedTempDir {
  std::filesystem::path path;

  ScopedTempDir()
      : path(std::filesystem::temp_directory_path() /
             ("ultra_symbol_query_" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path);
  }

  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

std::vector<std::string> canonicalDefinitions(
    const std::vector<ultra::engine::query::SymbolDefinition>& definitions) {
  std::vector<std::string> rows;
  rows.reserve(definitions.size());
  for (const ultra::engine::query::SymbolDefinition& definition : definitions) {
    rows.push_back(definition.filePath + "|" + std::to_string(definition.lineNumber) +
                   "|" + std::to_string(definition.symbolId) + "|" +
                   definition.signature);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

}  // namespace

TEST(SymbolQueryEngineTest, QueryFindsSymbolDefinition) {
  ultra::engine::query::SymbolQueryEngine engine;
  engine.rebuild(makeQueryState(false), 1U);

  const auto definitions = engine.findDefinition("coreFn");
  ASSERT_EQ(definitions.size(), 1U);
  EXPECT_EQ(definitions.front().filePath, "core.cpp");
  EXPECT_EQ(definitions.front().lineNumber, 10U);
  EXPECT_EQ(definitions.front().signature, "int coreFn()");
}

TEST(SymbolQueryEngineTest, QueryFindsSymbolReferences) {
  ultra::engine::query::SymbolQueryEngine engine;
  engine.rebuild(makeQueryState(false), 1U);

  const std::vector<std::string> references = engine.findReferences("coreFn");
  EXPECT_EQ(references, std::vector<std::string>({"service.cpp"}));
}

TEST(SymbolQueryEngineTest, QueryFindsFileDependencies) {
  ultra::engine::query::SymbolQueryEngine engine;
  engine.rebuild(makeQueryState(false), 1U);

  const std::vector<std::string> dependencies =
      engine.findFileDependencies("app.cpp");
  EXPECT_EQ(dependencies, std::vector<std::string>({"service.cpp"}));
}

TEST(SymbolQueryEngineTest, QueryFindsSymbolDependencies) {
  ultra::engine::query::SymbolQueryEngine engine;
  engine.rebuild(makeQueryState(false), 1U);

  const std::vector<std::string> dependencies =
      engine.findSymbolDependencies("serviceFn");
  EXPECT_EQ(dependencies, std::vector<std::string>({"coreFn"}));
}

TEST(SymbolQueryEngineTest, QueryImpactRegionTraversal) {
  ultra::engine::query::SymbolQueryEngine engine;
  engine.rebuild(makeQueryState(false), 1U);

  const std::vector<std::string> impact = engine.findImpactRegion("coreFn", 2U);
  EXPECT_EQ(impact,
            std::vector<std::string>(
                {"app.cpp", "core.cpp", "service.cpp", "worker.cpp"}));
}

TEST(SymbolQueryEngineTest, QueryResultsRemainDeterministic) {
  ultra::engine::query::SymbolQueryEngine engineA;
  ultra::engine::query::SymbolQueryEngine engineB;

  engineA.rebuild(makeQueryState(false), 7U);
  engineB.rebuild(makeQueryState(true), 7U);

  EXPECT_EQ(canonicalDefinitions(engineA.findDefinition("coreFn")),
            canonicalDefinitions(engineB.findDefinition("coreFn")));
  EXPECT_EQ(canonicalDefinitions(engineA.findDefinition("serviceFn")),
            canonicalDefinitions(engineB.findDefinition("serviceFn")));
  EXPECT_EQ(engineA.findReferences("coreFn"), engineB.findReferences("coreFn"));
  EXPECT_EQ(engineA.findReferences("serviceFn"),
            engineB.findReferences("serviceFn"));
  EXPECT_EQ(engineA.findFileDependencies("app.cpp"),
            engineB.findFileDependencies("app.cpp"));
  EXPECT_EQ(engineA.findSymbolDependencies("serviceFn"),
            engineB.findSymbolDependencies("serviceFn"));
  EXPECT_EQ(engineA.findImpactRegion("coreFn", 2U),
            engineB.findImpactRegion("coreFn", 2U));

  EXPECT_EQ(engineA.findImpactRegion("coreFn", 2U),
            engineA.findImpactRegion("coreFn", 2U));
}

TEST(SymbolQueryEngineTest, StateManagerIntegratesLayer6QueryEngine) {
  ultra::core::StateManager manager;
  manager.replaceState(makeQueryState(false));

  const auto definitions = manager.findDefinition("coreFn");
  ASSERT_EQ(definitions.size(), 1U);
  EXPECT_EQ(definitions.front().filePath, "core.cpp");

  EXPECT_EQ(manager.findReferences("coreFn"),
            std::vector<std::string>({"service.cpp"}));
  EXPECT_EQ(manager.findFileDependencies("app.cpp"),
            std::vector<std::string>({"service.cpp"}));
  EXPECT_EQ(manager.findSymbolDependencies("serviceFn"),
            std::vector<std::string>({"coreFn"}));
  EXPECT_EQ(manager.findImpactRegion("coreFn", 2U),
            std::vector<std::string>(
                {"app.cpp", "core.cpp", "service.cpp", "worker.cpp"}));
}

TEST(SymbolQueryEngineTest, QueriesWorkAfterPersistedGraphReload) {
  const ScopedTempDir tempDir;

  ultra::core::StateManager writer(tempDir.path);
  writer.replaceState(makeQueryState(false));

  std::string error;
  ASSERT_TRUE(writer.persistGraphStore(error)) << error;

  ultra::core::StateManager reader(tempDir.path);
  ASSERT_TRUE(reader.loadPersistedGraph(error)) << error;

  const auto definitions = reader.findDefinition("coreFn");
  ASSERT_EQ(definitions.size(), 1U);
  EXPECT_EQ(definitions.front().filePath, "core.cpp");

  EXPECT_EQ(reader.findReferences("coreFn"),
            std::vector<std::string>({"service.cpp"}));
  EXPECT_EQ(reader.findFileDependencies("app.cpp"),
            std::vector<std::string>({"service.cpp"}));
  EXPECT_EQ(reader.findSymbolDependencies("serviceFn"),
            std::vector<std::string>({"coreFn"}));
  EXPECT_EQ(reader.findImpactRegion("coreFn", 2U),
            std::vector<std::string>(
                {"app.cpp", "core.cpp", "service.cpp", "worker.cpp"}));
}
