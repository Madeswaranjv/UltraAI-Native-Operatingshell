#include <gtest/gtest.h>

#include "ai/Hashing.h"
#include "core/state_manager.h"
#include "runtime/precision_invalidation.h"
//E:\Projects\Ultra\tests\test_precision_invalidation.cpp
#include <algorithm>
#include <string>
#include <vector>

namespace {

ultra::ai::RuntimeState makeBodyChangeState() {
  ultra::ai::RuntimeState state;

  ultra::ai::FileRecord fileA;
  fileA.fileId = 1U;
  fileA.path = "a.cpp";
  fileA.hash = ultra::ai::sha256OfString("a_v1");
  fileA.lastModified = 10U;

  ultra::ai::FileRecord fileB;
  fileB.fileId = 2U;
  fileB.path = "b.cpp";
  fileB.hash = ultra::ai::sha256OfString("b_v1");
  fileB.lastModified = 10U;

  state.files = {fileA, fileB};

  ultra::ai::SymbolRecord foo;
  foo.symbolId = 1ULL;
  foo.fileId = 1U;
  foo.name = "foo";
  foo.signature = "int()";
  foo.symbolType = ultra::ai::SymbolType::Function;
  foo.visibility = ultra::ai::Visibility::Public;

  ultra::ai::SymbolRecord bar;
  bar.symbolId = 2ULL;
  bar.fileId = 2U;
  bar.name = "bar";
  bar.signature = "int()";
  bar.symbolType = ultra::ai::SymbolType::Function;
  bar.visibility = ultra::ai::Visibility::Public;

  state.symbols = {foo, bar};

  ultra::ai::SymbolNode fooNode;
  fooNode.name = "foo";
  fooNode.definedIn = "a.cpp";
  fooNode.weight = 1.0;
  fooNode.centrality = 0.2;
  state.symbolIndex["foo"] = fooNode;

  ultra::ai::SymbolNode barNode;
  barNode.name = "bar";
  barNode.definedIn = "b.cpp";
  barNode.weight = 1.0;
  barNode.centrality = 0.2;
  state.symbolIndex["bar"] = barNode;

  return state;
}

bool contains(const std::vector<std::string>& values, const std::string& target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

bool containsSymbol(const std::vector<ultra::runtime::SymbolID>& values,
                    const ultra::runtime::SymbolID target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

}  // namespace

TEST(PrecisionInvalidation, TimestampOnlyChangeWithStableHashDoesNotInvalidateFile) {
  ultra::ai::RuntimeState previous = makeBodyChangeState();
  ultra::ai::RuntimeState current = previous;
  current.files[0].lastModified = previous.files[0].lastModified + 100U;

  const ultra::runtime::DiffResult diff =
      ultra::runtime::buildDiffResult(previous, current, nullptr, 0U);

  EXPECT_TRUE(diff.changedFiles.empty());
  EXPECT_TRUE(diff.affectedSymbols.empty());
}

TEST(PrecisionInvalidation, BodyChangeClassificationUsesChangedFilesAndAffectedSymbols) {
  ultra::ai::RuntimeState previous = makeBodyChangeState();
  ultra::ai::RuntimeState current = previous;
  current.files[0].hash = ultra::ai::sha256OfString("a_v2");
  current.files[0].lastModified = previous.files[0].lastModified + 1U;

  const ultra::runtime::DiffResult diff =
      ultra::runtime::buildDiffResult(previous, current, nullptr, 0U);

  EXPECT_TRUE(contains(diff.changedFiles, "a.cpp"));
  EXPECT_TRUE(containsSymbol(diff.affectedSymbols, 1ULL));
  EXPECT_FALSE(containsSymbol(diff.affectedSymbols, 2ULL));
  EXPECT_EQ(ultra::runtime::classifyChange(diff),
            ultra::runtime::StructuralChangeType::BODY_CHANGE);
}

TEST(PrecisionInvalidation, BodyChangeMutationAvoidsFullSymbolGraphRebuild) {
  ultra::core::StateManager manager;
  manager.replaceState(makeBodyChangeState());

  const ultra::runtime::GraphSnapshot before = manager.getSnapshot();
  const auto symbolFooBefore = before.graph->getNode("symbol:1");
  const auto symbolBarBefore = before.graph->getNode("symbol:2");
  const auto fileABefore = before.graph->getNode("file:a.cpp");
  const auto fileBBefore = before.graph->getNode("file:b.cpp");
  ASSERT_FALSE(symbolFooBefore.nodeId.empty());
  ASSERT_FALSE(symbolBarBefore.nodeId.empty());
  ASSERT_FALSE(fileABefore.nodeId.empty());
  ASSERT_FALSE(fileBBefore.nodeId.empty());

  const ultra::core::KernelMutationOutcome outcome = manager.applyOverlayMutation(
      before,
      [](ultra::ai::RuntimeState& state, ultra::engine::WeightEngine& weightEngine,
         ultra::memory::LruManager& lruManager) {
        for (auto& file : state.files) {
          if (file.path == "a.cpp") {
            file.hash = ultra::ai::sha256OfString("a_v2");
            file.lastModified += 1U;
            weightEngine.incrementalModify(file.path);
            lruManager.touch(file.path);
            return true;
          }
        }
        return false;
      });
  ASSERT_TRUE(outcome.applied);
  EXPECT_FALSE(outcome.rolledBack);

  const ultra::runtime::GraphSnapshot after = manager.getSnapshot();
  const auto symbolFooAfter = after.graph->getNode("symbol:1");
  const auto symbolBarAfter = after.graph->getNode("symbol:2");
  const auto fileAAfter = after.graph->getNode("file:a.cpp");
  const auto fileBAfter = after.graph->getNode("file:b.cpp");
  ASSERT_FALSE(symbolFooAfter.nodeId.empty());
  ASSERT_FALSE(symbolBarAfter.nodeId.empty());
  ASSERT_FALSE(fileAAfter.nodeId.empty());
  ASSERT_FALSE(fileBAfter.nodeId.empty());

  EXPECT_EQ(symbolFooAfter.version, symbolFooBefore.version);
  EXPECT_EQ(symbolBarAfter.version, symbolBarBefore.version);
  EXPECT_GT(fileAAfter.version, fileABefore.version);
  EXPECT_EQ(fileBAfter.version, fileBBefore.version);
}
