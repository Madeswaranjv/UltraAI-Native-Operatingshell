#include <gtest/gtest.h>

#include "intelligence/BranchEvictionPolicy.h"
#include "intelligence/BranchStore.h"
#include "memory/CognitiveMemoryManager.h"
#include "memory/HotSlice.h"
#include "memory/StateGraph.h"
#include "memory/StateNode.h"
#include "runtime/GraphSnapshot.h"

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

ultra::memory::StateNode makeNode(const std::string& nodeId,
                                  const double centrality = 0.0) {
  ultra::memory::StateNode node;
  node.nodeId = nodeId;
  node.nodeType = ultra::memory::NodeType::Symbol;
  node.version = 1U;
  node.data = nlohmann::json::object();
  node.data["centrality"] = centrality;
  return node;
}

ultra::runtime::GraphSnapshot makeSnapshot(const std::uint64_t version) {
  ultra::runtime::GraphSnapshot snapshot;
  snapshot.graph = std::make_shared<ultra::memory::StateGraph>();
  snapshot.version = version;
  snapshot.branch = ultra::runtime::BranchId::nil();
  return snapshot;
}

}  // namespace

TEST(MemoryGovernance, HotSliceEvictionIsDeterministic) {
  ultra::memory::HotSlice firstSlice(3U);
  ultra::memory::HotSlice secondSlice(3U);

  const std::vector<ultra::memory::StateNode> nodes = {
      makeNode("symbol:alpha", 0.10),
      makeNode("symbol:beta", 0.10),
      makeNode("symbol:gamma", 0.10),
      makeNode("symbol:delta", 0.10)};

  for (const auto& node : nodes) {
    firstSlice.storeNode(node, 1U);
    secondSlice.storeNode(node, 1U);
  }

  EXPECT_FALSE(firstSlice.containsNode("symbol:gamma", 1U));
  EXPECT_FALSE(secondSlice.containsNode("symbol:gamma", 1U));
  EXPECT_EQ(firstSlice.currentSize(), 3U);
  EXPECT_EQ(secondSlice.currentSize(), 3U);

  const auto firstTop = firstSlice.getTopK(3U, 1U);
  const auto secondTop = secondSlice.getTopK(3U, 1U);
  ASSERT_EQ(firstTop.size(), secondTop.size());
  for (std::size_t index = 0; index < firstTop.size(); ++index) {
    EXPECT_EQ(firstTop[index].nodeId, secondTop[index].nodeId);
  }
}

TEST(MemoryGovernance, BranchEvictionRespectsUsageScore) {
  ultra::intelligence::BranchStore store;

  ultra::intelligence::Branch coldSeed;
  coldSeed.goal = "cold";
  ultra::intelligence::Branch coldBranch = store.create(coldSeed);
  coldBranch.lastMutationSequence = coldBranch.creationSequence;
  coldBranch.dependencyReferences = {"file:cold.cpp"};
  coldBranch.currentExecutionNodeId.clear();
  ASSERT_TRUE(store.update(coldBranch));

  ultra::intelligence::Branch hotSeed;
  hotSeed.goal = "hot";
  ultra::intelligence::Branch hotBranch = store.create(hotSeed);
  hotBranch.lastMutationSequence = hotBranch.creationSequence + 8U;
  hotBranch.dependencyReferences = {"file:a.cpp", "file:b.cpp", "file:c.cpp"};
  hotBranch.currentExecutionNodeId = "symbol:active";
  hotBranch.confidence.stabilityScore = 0.90;
  hotBranch.confidence.decisionReliabilityIndex = 0.90;
  hotBranch.confidence.riskAdjustedConfidence = 0.85;
  ASSERT_TRUE(store.update(hotBranch));

  store.setLruOrder({hotBranch.branchId, coldBranch.branchId});

  ultra::intelligence::BranchEvictionPolicy policy;
  EXPECT_EQ(policy.selectEvictionCandidate(store), coldBranch.branchId);
}

TEST(MemoryGovernance, TokenBudgetEnforcementWorks) {
  const std::filesystem::path root =
      std::filesystem::current_path() / "tmp_memory_governance_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  ultra::memory::CognitiveMemoryManager manager(root);
  const ultra::runtime::GraphSnapshot snapshot = makeSnapshot(17U);
  manager.bindToSnapshot(&snapshot);

  ultra::memory::PerformanceSnapshot performance;
  performance.avgTokenSavingsRatio = 0.05;
  performance.avgLatencyMs = 220.0;
  performance.errorRate = 0.20;
  performance.hotSliceHitRate = 0.20;
  performance.contextReuseRate = 0.15;
  performance.compressionRatio = 0.90;
  performance.overlayReuseRate = 0.10;
  performance.impactPredictionAccuracy = 0.35;

  manager.applyStrategicAdjustments(&performance);

  const std::size_t governedBudget = manager.governedTokenBudget(400U);
  const auto& governance = manager.governanceState();

  EXPECT_LT(governedBudget, 400U);
  EXPECT_GE(governedBudget, 64U);
  EXPECT_GE(governance.compressionDepth, 2U);
  EXPECT_LT(governance.tokenBudgetScale, 1.0);

  std::filesystem::remove_all(root, ec);
}
