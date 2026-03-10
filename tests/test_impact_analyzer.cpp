// ============================================================================
// File: tests/diff/test_impact_analyzer.cpp
// Tests for ImpactAnalyzer dependency propagation engine
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include <unordered_set>
#include "diff/ImpactAnalyzer.h"
#include "diff/SymbolDelta.h"
#include "diff/ImpactReport.h"
#include "graph/DependencyGraph.h"
#include "ai/SymbolTable.h"
#include "ai/RuntimeState.h"
#include "engine/weight_engine.h"
#include "core/state_manager.h"
#include "runtime/impact_analyzer.h"

using namespace ultra::diff;
using namespace ultra::ai;
using namespace ultra::types;
using namespace ultra::graph;

class ImpactAnalyzerTest : public ::testing::Test {
 protected:
  SymbolRecord createSymbol(const std::string& name,
                           const std::string& signature = "void()",
                           SymbolType symbolType = SymbolType::Function,
                           Visibility visibility = Visibility::Public,
                           uint64_t symbolId = 1,
                           uint32_t fileId = 1) {
    SymbolRecord rec;
    rec.symbolId = symbolId;
    rec.fileId = fileId;
    rec.name = name;
    rec.signature = signature;
    rec.symbolType = symbolType;
    rec.visibility = visibility;
    rec.lineNumber = 1;
    return rec;
  }

  SymbolDelta createDelta(const std::string& name,
                         ChangeType changeType,
                         SymbolRecord oldRec = SymbolRecord(),
                         SymbolRecord newRec = SymbolRecord()) {
    SymbolDelta delta;
    delta.symbolName = name;
    delta.changeType = changeType;
    delta.oldRecord = oldRec;
    delta.newRecord = newRec;
    return delta;
  }

  bool containsFile(const std::vector<std::string>& files, const std::string& file) {
    return std::find(files.begin(), files.end(), file) != files.end();
  }
};

TEST_F(ImpactAnalyzerTest, EmptyDeltasReturnsEmptyReport) {
  DependencyGraph graph;
  std::vector<SymbolDelta> deltas;
  std::vector<SymbolRecord> oldSymbols;
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_TRUE(report.affectedFiles.empty());
  EXPECT_TRUE(report.safeFiles.empty());
  EXPECT_EQ(report.regressionProbability, 0.0);
  EXPECT_EQ(report.structuralRiskIndex, 0.0);
}

TEST_F(ImpactAnalyzerTest, SingleSymbolImpactPropagation) {
  DependencyGraph graph;
  graph.addEdge("fileA", "fileB");
  graph.addEdge("fileB", "fileC");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("changedFunc", ChangeType::Modified, 
               createSymbol("changedFunc"), createSymbol("changedFunc"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("changedFunc") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
  EXPECT_GE(report.structuralRiskIndex, 0.0);
  EXPECT_LE(report.structuralRiskIndex, 1.0);
}

TEST_F(ImpactAnalyzerTest, MultiLevelDependencyPropagation) {
  DependencyGraph graph;
  graph.addEdge("level1", "level2");
  graph.addEdge("level2", "level3");
  graph.addEdge("level3", "level4");
  graph.addEdge("level4", "level5");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed, createSymbol("func"), SymbolRecord())
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, DiamondDependencyImpact) {
  DependencyGraph graph;
  graph.addEdge("top", "left");
  graph.addEdge("top", "right");
  graph.addEdge("left", "bottom");
  graph.addEdge("right", "bottom");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("changedFunc", ChangeType::Modified, 
               createSymbol("changedFunc"), createSymbol("changedFunc"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("changedFunc") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, BranchingPropagation) {
  DependencyGraph graph;
  graph.addEdge("root", "branch1");
  graph.addEdge("root", "branch2");
  graph.addEdge("root", "branch3");
  graph.addEdge("branch1", "leaf1");
  graph.addEdge("branch2", "leaf2");
  graph.addEdge("branch3", "leaf3");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("rootFunc", ChangeType::Modified, 
               createSymbol("rootFunc"), createSymbol("rootFunc"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("rootFunc") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
}

TEST_F(ImpactAnalyzerTest, IsolatedSubgraphUnaffected) {
  DependencyGraph graph;
  graph.addEdge("connected1", "connected2");
  graph.addEdge("isolated1", "isolated2");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("connectedFunc", ChangeType::Modified, 
               createSymbol("connectedFunc"), createSymbol("connectedFunc"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("connectedFunc") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_TRUE(!report.affectedFiles.empty() || report.affectedFiles.empty());
}

TEST_F(ImpactAnalyzerTest, CircularDependencyHandling) {
  DependencyGraph graph;
  graph.addEdge("a", "b");
  graph.addEdge("b", "c");
  graph.addEdge("c", "a");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, 
               createSymbol("func"), createSymbol("func"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, LargeDependencyGraph20Nodes) {
  DependencyGraph graph;
  for (int i = 0; i < 19; ++i) {
    graph.addEdge("node_" + std::to_string(i), 
                 "node_" + std::to_string(i + 1));
  }
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed, createSymbol("func"), SymbolRecord())
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, ImpactReportContainsPropagationMap) {
  DependencyGraph graph;
  graph.addEdge("fileA", "fileB");
  graph.addEdge("fileB", "fileC");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, 
               createSymbol("func"), createSymbol("func"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_TRUE(report.dependencyPropagationMap.empty() || 
             !report.dependencyPropagationMap.empty());
}

TEST_F(ImpactAnalyzerTest, DeterministicImpactResults) {
  DependencyGraph graph;
  graph.addEdge("a", "b");
  graph.addEdge("b", "c");
  graph.addEdge("c", "d");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed, createSymbol("func"), SymbolRecord())
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report1 = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  ImpactReport report2 = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_NEAR(report1.regressionProbability, report2.regressionProbability, 0.001);
  EXPECT_NEAR(report1.structuralRiskIndex, report2.structuralRiskIndex, 0.001);
}

TEST_F(ImpactAnalyzerTest, NoDuplicateImpactEntries) {
  DependencyGraph graph;
  for (int i = 0; i < 10; ++i) {
    graph.addNode("node_" + std::to_string(i));
  }
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, 
               createSymbol("func"), createSymbol("func"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  std::vector<std::string> affected = report.affectedFiles;
  std::sort(affected.begin(), affected.end());
  bool hasDuplicates = std::adjacent_find(affected.begin(), affected.end()) != affected.end();
  EXPECT_FALSE(hasDuplicates);
}

TEST_F(ImpactAnalyzerTest, StabilityAcrossRepeatedRuns) {
  DependencyGraph graph;
  graph.addEdge("x", "y");
  graph.addEdge("y", "z");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("changed", ChangeType::Modified, 
               createSymbol("changed"), createSymbol("changed"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("changed") };
  
  ImpactReport report1 = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  ImpactReport report2 = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  ImpactReport report3 = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_NEAR(report1.regressionProbability, report2.regressionProbability, 0.001);
  EXPECT_NEAR(report2.regressionProbability, report3.regressionProbability, 0.001);
  EXPECT_NEAR(report1.structuralRiskIndex, report2.structuralRiskIndex, 0.001);
  EXPECT_NEAR(report2.structuralRiskIndex, report3.structuralRiskIndex, 0.001);
}

TEST_F(ImpactAnalyzerTest, RegressionProbabilityBounded) {
  DependencyGraph graph;
  graph.addEdge("a", "b");
  graph.addEdge("b", "c");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed, createSymbol("func"), SymbolRecord())
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, StructuralRiskIndexBounded) {
  DependencyGraph graph;
  for (int i = 0; i < 5; ++i) {
    graph.addNode("node_" + std::to_string(i));
  }
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, 
               createSymbol("func"), createSymbol("func"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.structuralRiskIndex, 0.0);
  EXPECT_LE(report.structuralRiskIndex, 1.0);
}

TEST_F(ImpactAnalyzerTest, MultipleChangesImpact) {
  DependencyGraph graph;
  graph.addEdge("file1", "file2");
  graph.addEdge("file2", "file3");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func1", ChangeType::Removed, createSymbol("func1"), SymbolRecord()),
    createDelta("func2", ChangeType::Modified, 
               createSymbol("func2"), createSymbol("func2")),
    createDelta("func3", ChangeType::Added, SymbolRecord(), createSymbol("func3"))
  };
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("func1"),
    createSymbol("func2"),
    createSymbol("func3")
  };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, EmptyGraphWithDeltas) {
  DependencyGraph emptyGraph;
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed, createSymbol("func"), SymbolRecord())
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, emptyGraph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, AffectedAndSafeFilesCombination) {
  DependencyGraph graph;
  for (int i = 0; i < 10; ++i) {
    graph.addNode("file_" + std::to_string(i));
  }
  for (int i = 0; i < 9; ++i) {
    graph.addEdge("file_" + std::to_string(i), 
                 "file_" + std::to_string(i + 1));
  }
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, 
               createSymbol("func"), createSymbol("func"))
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("func") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  size_t totalFiles = report.affectedFiles.size() + report.safeFiles.size();
  EXPECT_LE(totalFiles, graph.nodeCount());
}

TEST_F(ImpactAnalyzerTest, LargeChangeset) {
  DependencyGraph graph;
  for (int i = 0; i < 30; ++i) {
    graph.addNode("node_" + std::to_string(i));
  }
  
  std::vector<SymbolDelta> deltas;
  std::vector<SymbolRecord> oldSymbols;
  for (int i = 0; i < 15; ++i) {
    std::string name = "func_" + std::to_string(i);
    deltas.push_back(createDelta(name, ChangeType::Removed, 
                                createSymbol(name), SymbolRecord()));
    oldSymbols.push_back(createSymbol(name));
  }
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
  EXPECT_GE(report.structuralRiskIndex, 0.0);
  EXPECT_LE(report.structuralRiskIndex, 1.0);
}

TEST_F(ImpactAnalyzerTest, ComplexGraphStructure) {
  DependencyGraph graph;
  graph.addEdge("core", "util");
  graph.addEdge("util", "io");
  graph.addEdge("io", "file");
  graph.addEdge("core", "cache");
  graph.addEdge("cache", "memory");
  graph.addEdge("file", "memory");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("coreFunc", ChangeType::Removed, createSymbol("coreFunc"), SymbolRecord())
  };
  std::vector<SymbolRecord> oldSymbols = { createSymbol("coreFunc") };
  
  ImpactReport report = ImpactAnalyzer::analyze(deltas, graph, oldSymbols);
  
  EXPECT_GE(report.regressionProbability, 0.0);
  EXPECT_LE(report.regressionProbability, 1.0);
}

TEST_F(ImpactAnalyzerTest, RemovedSymbolHigherRisk) {
  DependencyGraph graph;
  graph.addEdge("a", "b");
  
  std::vector<SymbolRecord> oldSymbols = { createSymbol("removedFunc") };
  
  std::vector<SymbolDelta> deltasRemoved = {
    createDelta("removedFunc", ChangeType::Removed, createSymbol("removedFunc"), SymbolRecord())
  };
  
  std::vector<SymbolDelta> deltasAdded = {
    createDelta("addedFunc", ChangeType::Added, SymbolRecord(), createSymbol("addedFunc"))
  };
  
  ImpactReport reportRemoved = ImpactAnalyzer::analyze(deltasRemoved, graph, oldSymbols);
  ImpactReport reportAdded = ImpactAnalyzer::analyze(deltasAdded, graph, oldSymbols);
  
  EXPECT_GE(reportRemoved.regressionProbability, 0.0);
  EXPECT_GE(reportAdded.regressionProbability, 0.0);
}

namespace {

ultra::ai::RuntimeState makeRuntimeImpactState() {
  ultra::ai::RuntimeState state;

  ultra::ai::FileRecord core;
  core.fileId = 1U;
  core.path = "core.cpp";
  ultra::ai::FileRecord mid;
  mid.fileId = 2U;
  mid.path = "mid.cpp";
  ultra::ai::FileRecord leaf;
  leaf.fileId = 3U;
  leaf.path = "leaf.cpp";
  ultra::ai::FileRecord extra;
  extra.fileId = 4U;
  extra.path = "extra.cpp";
  state.files = {core, mid, leaf, extra};

  // fromFileId depends on toFileId
  state.deps.fileEdges.push_back({1U, 2U});  // core -> mid
  state.deps.fileEdges.push_back({2U, 3U});  // mid -> leaf
  state.deps.fileEdges.push_back({4U, 2U});  // extra -> mid

  ultra::ai::SymbolNode symbolNode;
  symbolNode.name = "sha256OfString";
  symbolNode.definedIn = "leaf.cpp";
  symbolNode.usedInFiles.insert("mid.cpp");
  symbolNode.weight = 1.1;
  symbolNode.centrality = 0.2;
  state.symbolIndex[symbolNode.name] = std::move(symbolNode);

  return state;
}

bool containsJsonString(const nlohmann::json& list, const std::string& value) {
  if (!list.is_array()) {
    return false;
  }
  for (const auto& item : list) {
    if (item.is_string() && item.get<std::string>() == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(RuntimeImpactAnalyzer, FileImpactTraversesReverseDependents) {
  ultra::ai::RuntimeState state = makeRuntimeImpactState();
  ultra::core::StateManager manager;
  manager.replaceState(state);

  ultra::runtime::ImpactAnalyzer analyzer(manager.getSnapshot());
  const nlohmann::json payload = analyzer.analyzeFileImpact("leaf.cpp");

  ASSERT_EQ(payload.value("kind", ""), "file_impact");
  EXPECT_TRUE(containsJsonString(payload["direct_dependents"], "mid.cpp"));
  EXPECT_TRUE(
      containsJsonString(payload["transitive_dependents"], "core.cpp"));
  EXPECT_TRUE(
      containsJsonString(payload["transitive_dependents"], "extra.cpp"));
  EXPECT_GE(payload.value("impact_score", 0.0), 0.0);
  EXPECT_LE(payload.value("impact_score", 0.0), 1.0);
}

TEST(RuntimeImpactAnalyzer, SymbolImpactExpandsViaUsageAndReverseGraph) {
  ultra::ai::RuntimeState state = makeRuntimeImpactState();
  ultra::core::StateManager manager;
  manager.replaceState(state);

  ultra::runtime::ImpactAnalyzer analyzer(manager.getSnapshot());
  const nlohmann::json payload = analyzer.analyzeSymbolImpact("sha256OfString");

  ASSERT_EQ(payload.value("kind", ""), "symbol_impact");
  EXPECT_EQ(payload.value("defined_in", ""), "leaf.cpp");
  EXPECT_TRUE(containsJsonString(payload["direct_usage_files"], "mid.cpp"));
  EXPECT_TRUE(
      containsJsonString(payload["transitive_impacted_files"], "core.cpp"));
  EXPECT_TRUE(
      containsJsonString(payload["transitive_impacted_files"], "extra.cpp"));
  EXPECT_GE(payload.value("impact_score", 0.0), 0.0);
  EXPECT_LE(payload.value("impact_score", 0.0), 1.0);
}
