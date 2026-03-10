#include <gtest/gtest.h>

#include "memory/CognitiveMemoryManager.h"
#include "memory/StateGraph.h"
#include "runtime/GraphSnapshot.h"
//E:\Projects\Ultra\tests\test_cognitive_memory.cpp
#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>

namespace {

ultra::runtime::GraphSnapshot makeSnapshot(std::uint64_t version) {
  ultra::runtime::GraphSnapshot snapshot;
  snapshot.graph = std::make_shared<ultra::memory::StateGraph>();
  snapshot.version = version;
  snapshot.branch = ultra::runtime::BranchId::nil();
  return snapshot;
}

}  // namespace

TEST(CognitiveMemoryManager, PersistsIdentityAndEpisodicLog) {
  const std::filesystem::path root =
      std::filesystem::current_path() / "tmp_cognitive_memory_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  ultra::memory::CognitiveMemoryManager manager(root);
  const std::string instanceId = manager.identity().instanceId;
  const std::uint64_t deterministicSeed = manager.identity().deterministicSeed;

  const ultra::runtime::GraphSnapshot snapshot = makeSnapshot(7U);
  manager.recordIntentExecution("intent:test", snapshot, true, false, "ok");

  ultra::memory::CognitiveMemoryManager reloaded(root);
  EXPECT_EQ(reloaded.identity().instanceId, instanceId);
  EXPECT_EQ(reloaded.identity().deterministicSeed, deterministicSeed);

  const auto events = reloaded.episodic.getEventsForVersion(7U);
  EXPECT_FALSE(events.empty());

  std::filesystem::remove_all(root, ec);
}

TEST(CognitiveMemoryManager, TracksSemanticEvolution) {
  const std::filesystem::path root =
      std::filesystem::current_path() / "tmp_semantic_memory_test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  ultra::memory::CognitiveMemoryManager manager(root);
  manager.updateSemanticEvolution("symbol:11", "alpha", "void()", "created", 1U);
  manager.updateSemanticEvolution("symbol:11", "alphaRenamed", "void()",
                                  "rename", 2U, "symbol:11");

  const std::string stable = manager.semantic.resolveStableIdentity("symbol:11");
  EXPECT_FALSE(stable.empty());
  const auto history = manager.semantic.getSymbolHistory(stable);
  EXPECT_EQ(history.size(), 2U);
  EXPECT_EQ(history.back().symbolName, "alphaRenamed");

  std::filesystem::remove_all(root, ec);
}
