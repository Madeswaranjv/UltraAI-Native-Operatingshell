#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "core/state_manager.h"
#include "engine/context/ContextBuilder.h"
#include "engine/context_compression/ContextHierarchyBuilder.h"

#include <external/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
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

void addDefinition(ultra::ai::RuntimeState& state,
                   const std::uint32_t fileId,
                   const std::uint32_t localIndex,
                   const std::string& path,
                   const std::string& name,
                   const std::string& signature,
                   const std::uint32_t lineNumber,
                   const std::vector<std::string>& usedIn,
                   const double weight,
                   const double centrality) {
  state.symbols.push_back(makeSymbol(
      fileId, localIndex, name, signature, ultra::ai::SymbolType::Function, lineNumber));

  ultra::ai::SymbolNode node;
  node.name = name;
  node.definedIn = path;
  node.usedInFiles.insert(usedIn.begin(), usedIn.end());
  node.weight = weight;
  node.centrality = centrality;
  state.symbolIndex[name] = std::move(node);
}

ultra::ai::RuntimeState makeCompressionState(const bool shuffled) {
  ultra::ai::RuntimeState state;

  const auto addFile = [&state](const std::uint32_t fileId,
                                const std::string& path) {
    ultra::ai::FileRecord file;
    file.fileId = fileId;
    file.path = path;
    state.files.push_back(std::move(file));
  };

  addFile(1U, "src/core/core.cpp");
  addFile(2U, "src/service/service.cpp");
  addFile(3U, "src/runtime/app.cpp");
  addFile(4U, "src/runtime/worker.cpp");
  addFile(5U, "src/api/api.cpp");
  addFile(6U, "src/cli/cli.cpp");

  addDefinition(state,
                1U,
                1U,
                "src/core/core.cpp",
                "coreFn",
                "int coreFn()",
                11U,
                {"src/service/service.cpp",
                 "src/runtime/app.cpp",
                 "src/runtime/worker.cpp",
                 "src/api/api.cpp",
                 "src/cli/cli.cpp"},
                1.25,
                0.85);
  addDefinition(state,
                2U,
                1U,
                "src/service/service.cpp",
                "serviceFn",
                "int serviceFn()",
                21U,
                {"src/runtime/app.cpp",
                 "src/runtime/worker.cpp",
                 "src/api/api.cpp",
                 "src/cli/cli.cpp"},
                1.15,
                0.72);
  addDefinition(state,
                3U,
                1U,
                "src/runtime/app.cpp",
                "appMain",
                "int appMain()",
                31U,
                {"src/cli/cli.cpp"},
                1.05,
                0.60);
  addDefinition(state,
                4U,
                1U,
                "src/runtime/worker.cpp",
                "workerTask",
                "int workerTask()",
                41U,
                {"src/api/api.cpp", "src/cli/cli.cpp"},
                0.95,
                0.57);
  addDefinition(state,
                5U,
                1U,
                "src/api/api.cpp",
                "apiHandle",
                "int apiHandle()",
                51U,
                {"src/cli/cli.cpp"},
                0.90,
                0.49);
  addDefinition(state,
                6U,
                1U,
                "src/cli/cli.cpp",
                "cliEntry",
                "int cliEntry()",
                61U,
                {},
                0.88,
                0.45);

  state.deps.fileEdges = {
      {3U, 2U},
      {4U, 2U},
      {5U, 2U},
      {6U, 5U},
      {6U, 3U},
      {2U, 1U},
      {5U, 1U},
  };
  state.deps.symbolEdges = {
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(3U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(4U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(5U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(6U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(5U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(6U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(3U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(5U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
  };

  if (shuffled) {
    std::reverse(state.files.begin(), state.files.end());
    std::reverse(state.symbols.begin(), state.symbols.end());
    std::reverse(state.deps.fileEdges.begin(), state.deps.fileEdges.end());
    std::reverse(state.deps.symbolEdges.begin(), state.deps.symbolEdges.end());
  }

  return state;
}

std::vector<std::uint64_t> extractNodeIds(const nlohmann::json& payload) {
  std::vector<std::uint64_t> ids;
  if (!payload.contains("nodes") || !payload["nodes"].is_array()) {
    return ids;
  }
  for (const auto& node : payload["nodes"]) {
    if (node.is_object() && node.contains("id")) {
      ids.push_back(node["id"].get<std::uint64_t>());
    }
  }
  return ids;
}

}  // namespace

TEST(ContextCompression, CompressedContextRespectsTokenBudget) {
  ultra::core::StateManager manager;
  manager.replaceState(makeCompressionState(false));

  const ultra::runtime::CognitiveState cognitiveState =
      manager.createCognitiveState(220U);
  ultra::engine::context::ContextBuilder builder(
      *cognitiveState.snapshot.runtimeState,
      cognitiveState.snapshot.graphStore,
      cognitiveState.snapshot.version,
      {.enableCompression = true, .graphSnapshot = &cognitiveState.snapshot});

  const auto slice = builder.buildImpactContext("coreFn", 220U, {}, 3U);
  const nlohmann::json payload = nlohmann::json::parse(slice.json);

  ASSERT_TRUE(payload.contains("metadata"));
  ASSERT_TRUE(payload["metadata"].contains("compression"));
  EXPECT_TRUE(payload["metadata"]["compression"].value("enabled", false));
  EXPECT_LE(slice.estimatedTokens, 220U);
}

TEST(ContextCompression, CompressionIsDeterministic) {
  ultra::core::StateManager manager;
  manager.replaceState(makeCompressionState(false));

  const ultra::runtime::CognitiveState cognitiveState =
      manager.createCognitiveState(320U);
  ultra::engine::context::ContextBuilder builder(
      *cognitiveState.snapshot.runtimeState,
      cognitiveState.snapshot.graphStore,
      cognitiveState.snapshot.version,
      {.enableCompression = true, .graphSnapshot = &cognitiveState.snapshot});

  const auto first = builder.buildImpactContext("coreFn", 320U, {}, 3U);
  const auto second = builder.buildImpactContext("coreFn", 320U, {}, 3U);

  EXPECT_EQ(first.json, second.json);
  EXPECT_EQ(first.includedNodes, second.includedNodes);
  EXPECT_EQ(first.estimatedTokens, second.estimatedTokens);
}

TEST(ContextCompression, HierarchyGenerationIsStable) {
  ultra::core::StateManager managerA;
  ultra::core::StateManager managerB;
  managerA.replaceState(makeCompressionState(false));
  managerB.replaceState(makeCompressionState(true));

  const ultra::runtime::CognitiveState stateA = managerA.createCognitiveState(512U);
  const ultra::runtime::CognitiveState stateB = managerB.createCognitiveState(512U);

  ultra::engine::context::ContextBuilder builderA(*stateA.snapshot.runtimeState,
                                                  stateA.snapshot.graphStore,
                                                  stateA.snapshot.version);
  ultra::engine::context::ContextBuilder builderB(*stateB.snapshot.runtimeState,
                                                  stateB.snapshot.graphStore,
                                                  stateB.snapshot.version);

  const auto sliceA = builderA.buildImpactContext("coreFn", 512U, {}, 3U);
  const auto sliceB = builderB.buildImpactContext("coreFn", 512U, {}, 3U);

  ultra::engine::context_compression::ContextHierarchyBuilder hierarchyBuilder;
  const auto hierarchyA = hierarchyBuilder.buildHierarchy(stateA.snapshot, sliceA);
  const auto hierarchyB = hierarchyBuilder.buildHierarchy(stateB.snapshot, sliceB);

  EXPECT_EQ(hierarchyA.dump(), hierarchyB.dump());

  ASSERT_TRUE(hierarchyA.contains("repository"));
  ASSERT_TRUE(hierarchyA["repository"].contains("modules"));
  std::vector<std::string> modulePaths;
  for (const auto& module : hierarchyA["repository"]["modules"]) {
    modulePaths.push_back(module.value("path", std::string{}));
  }
  EXPECT_TRUE(std::is_sorted(modulePaths.begin(), modulePaths.end()));
}

TEST(ContextCompression, CompressionPreservesNodeIdentity) {
  ultra::core::StateManager manager;
  manager.replaceState(makeCompressionState(false));

  const ultra::runtime::CognitiveState cognitiveState =
      manager.createCognitiveState(512U);
  ultra::engine::context::ContextBuilder rawBuilder(*cognitiveState.snapshot.runtimeState,
                                                    cognitiveState.snapshot.graphStore,
                                                    cognitiveState.snapshot.version);
  ultra::engine::context::ContextBuilder compressedBuilder(
      *cognitiveState.snapshot.runtimeState,
      cognitiveState.snapshot.graphStore,
      cognitiveState.snapshot.version,
      {.enableCompression = true, .graphSnapshot = &cognitiveState.snapshot});

  const auto raw = rawBuilder.buildImpactContext("coreFn", 512U, {}, 3U);
  const auto compressed =
      compressedBuilder.buildImpactContext("coreFn", 512U, {}, 3U);
  const nlohmann::json payload = nlohmann::json::parse(compressed.json);
  const std::vector<std::uint64_t> nodeIds = extractNodeIds(payload);

  EXPECT_EQ(raw.includedNodes, compressed.includedNodes);
  EXPECT_EQ(nodeIds, compressed.includedNodes);
}
