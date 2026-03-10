#include <gtest/gtest.h>

#include "ai/RuntimeState.h"
#include "core/state_manager.h"
#include "runtime/ContextExtractor.h"

#include <external/json.hpp>

#include <string>
#include <vector>

namespace {

ultra::ai::RuntimeState makeContextState() {
  ultra::ai::RuntimeState state;

  ultra::ai::FileRecord a;
  a.fileId = 1U;
  a.path = "src/a.cpp";
  ultra::ai::FileRecord b;
  b.fileId = 2U;
  b.path = "src/b.cpp";
  ultra::ai::FileRecord c;
  c.fileId = 3U;
  c.path = "src/c.cpp";
  ultra::ai::FileRecord d;
  d.fileId = 4U;
  d.path = "src/d.cpp";
  state.files = {a, b, c, d};

  state.deps.fileEdges.push_back({1U, 2U});
  state.deps.fileEdges.push_back({2U, 3U});
  state.deps.fileEdges.push_back({4U, 2U});

  const auto addSymbol =
      [&state](const std::uint64_t symbolId,
               const std::uint32_t fileId,
               const std::string& name,
               const std::string& definedIn,
               const std::vector<std::string>& usedIn,
               const double weight,
               const double centrality) {
        ultra::ai::SymbolRecord record;
        record.symbolId = symbolId;
        record.fileId = fileId;
        record.name = name;
        state.symbols.push_back(record);

        ultra::ai::SymbolNode node;
        node.name = name;
        node.definedIn = definedIn;
        node.usedInFiles.insert(usedIn.begin(), usedIn.end());
        node.weight = weight;
        node.centrality = centrality;
        state.symbolIndex[name] = std::move(node);
      };

  addSymbol(101ULL, 1U, "alpha", "src/a.cpp", {"src/b.cpp", "src/d.cpp"}, 1.1,
            0.6);
  addSymbol(102ULL, 2U, "beta", "src/b.cpp", {"src/a.cpp", "src/c.cpp"}, 0.9,
            0.5);
  addSymbol(103ULL, 3U, "gamma", "src/c.cpp", {"src/b.cpp"}, 0.8, 0.4);
  addSymbol(104ULL, 4U, "delta", "src/d.cpp", {"src/b.cpp"}, 0.7, 0.3);
  addSymbol(105ULL, 2U, "epsilon", "src/b.cpp", {"src/d.cpp"}, 0.6, 0.2);

  return state;
}

}  // namespace

TEST(ContextExtractor, DeterministicOutputForSameSnapshotAndQuery) {
  ultra::core::StateManager manager;
  manager.replaceState(makeContextState());
  const ultra::runtime::CognitiveState cognitiveState =
      manager.createCognitiveState(256U);

  ultra::runtime::ContextExtractor extractor;
  ultra::runtime::Query query;
  query.kind = ultra::runtime::QueryKind::Symbol;
  query.target = "alpha";
  query.impactDepth = 2U;

  const ultra::runtime::ContextSlice first =
      extractor.getMinimalContext(cognitiveState, query);
  const ultra::runtime::ContextSlice second =
      extractor.getMinimalContext(cognitiveState, query);

  EXPECT_EQ(first.json, second.json);
  EXPECT_EQ(first.includedNodes, second.includedNodes);
  EXPECT_EQ(first.estimatedTokens, second.estimatedTokens);
}

TEST(ContextExtractor, EnforcesTokenBudgetDeterministically) {
  ultra::core::StateManager manager;
  manager.replaceState(makeContextState());
  const ultra::runtime::CognitiveState relaxedState =
      manager.createCognitiveState(256U);
  const ultra::runtime::CognitiveState tightState =
      manager.createCognitiveState(128U);

  ultra::runtime::ContextExtractor extractor;
  ultra::runtime::Query query;
  query.kind = ultra::runtime::QueryKind::Impact;
  query.target = "src/c.cpp";
  query.impactDepth = 3U;

  const ultra::runtime::ContextSlice relaxed =
      extractor.getMinimalContext(relaxedState, query);
  const ultra::runtime::ContextSlice tight =
      extractor.getMinimalContext(tightState, query);

  EXPECT_LE(tight.estimatedTokens, 128U);
  EXPECT_LE(tight.includedNodes.size(), relaxed.includedNodes.size());

  const nlohmann::json parsed = nlohmann::json::parse(tight.json);
  ASSERT_TRUE(parsed.contains("nodes"));
  ASSERT_TRUE(parsed["nodes"].is_array());

  std::uint64_t previousId = 0U;
  bool firstNode = true;
  for (const auto& node : parsed["nodes"]) {
    ASSERT_TRUE(node.contains("id"));
    const std::uint64_t id = node["id"].get<std::uint64_t>();
    if (!firstNode) {
      EXPECT_LE(previousId, id);
    }
    previousId = id;
    firstNode = false;
  }
}
