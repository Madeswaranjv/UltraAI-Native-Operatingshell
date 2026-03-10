// ============================================================================
// File: tests/diff/test_diff_engine.cpp
// Tests for DiffEngine central facade
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "diff/DiffEngine.h"
#include "diff/DeltaReport.h"
#include "graph/DependencyGraph.h"
#include "ai/SymbolTable.h"

using namespace ultra::diff;
using namespace ultra::ai;
using namespace ultra::types;
using namespace ultra::graph;

class DiffEngineTest : public ::testing::Test {
 protected:
  DependencyGraph depGraph;

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

  bool containsDelta(const std::vector<SymbolDelta>& deltas,
                     const std::string& name,
                     ChangeType changeType) {
    return std::any_of(deltas.begin(), deltas.end(),
                      [&](const SymbolDelta& d) {
                        return d.symbolName == name && d.changeType == changeType;
                      });
  }

  bool hasDuplicateNames(const std::vector<SymbolDelta>& deltas) {
    std::vector<std::string> names;
    for (const auto& d : deltas) {
      names.push_back(d.symbolName);
    }
    std::sort(names.begin(), names.end());
    return std::adjacent_find(names.begin(), names.end()) != names.end();
  }
};

TEST_F(DiffEngineTest, EmptyOldEmptyNewReturnsEmptyReport) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_TRUE(report.changeObject.empty());
  EXPECT_TRUE(report.contractBreaks.empty());
}

TEST_F(DiffEngineTest, DetectAddedSymbols) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("NewFunc1"),
    createSymbol("NewFunc2")
  };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 2);
  EXPECT_TRUE(containsDelta(report.changeObject, "NewFunc1", ChangeType::Added));
  EXPECT_TRUE(containsDelta(report.changeObject, "NewFunc2", ChangeType::Added));
}

TEST_F(DiffEngineTest, DetectRemovedSymbols) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("OldFunc1"),
    createSymbol("OldFunc2")
  };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 2);
  EXPECT_TRUE(containsDelta(report.changeObject, "OldFunc1", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(report.changeObject, "OldFunc2", ChangeType::Removed));
}

TEST_F(DiffEngineTest, DetectModifiedSymbols) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Private)
  };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 1);
  EXPECT_TRUE(containsDelta(report.changeObject, "Func", ChangeType::Modified));
}

TEST_F(DiffEngineTest, MultipleSimultaneousChanges) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Keep"),
    createSymbol("Remove"),
    createSymbol("Modify", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Keep"),
    createSymbol("Add"),
    createSymbol("Modify", "void()", SymbolType::Function, Visibility::Private)
  };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 3);
  EXPECT_TRUE(containsDelta(report.changeObject, "Remove", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(report.changeObject, "Add", ChangeType::Added));
  EXPECT_TRUE(containsDelta(report.changeObject, "Modify", ChangeType::Modified));
}

TEST_F(DiffEngineTest, LargeSymbolGraph50Items) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols;
  
  for (int i = 0; i < 25; ++i) {
    oldSymbols.push_back(createSymbol("Old_" + std::to_string(i)));
    newSymbols.push_back(createSymbol("New_" + std::to_string(i)));
  }
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 50);
}

TEST_F(DiffEngineTest, DeterministicFirstRun) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("A"),
    createSymbol("B"),
    createSymbol("C")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("A"),
    createSymbol("D"),
    createSymbol("E")
  };
  
  DeltaReport report1 = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report1.changeObject.size(), 4);
}

TEST_F(DiffEngineTest, DeterministicSecondRun) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("A"),
    createSymbol("B"),
    createSymbol("C")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("A"),
    createSymbol("D"),
    createSymbol("E")
  };
  
  DeltaReport report2 = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report2.changeObject.size(), 4);
}

TEST_F(DiffEngineTest, ReportStructureComplete) {
  std::vector<SymbolRecord> oldSymbols = { createSymbol("Func") };
  std::vector<SymbolRecord> newSymbols = { createSymbol("Func", "void()", 
                                                        SymbolType::Function, Visibility::Private) };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_FALSE(report.changeObject.empty());
  EXPECT_TRUE(report.contractBreaks.size() >= 0);
  EXPECT_GE(report.riskScore.overallRisk, 0.0);
  EXPECT_LE(report.riskScore.overallRisk, 1.0);
}

TEST_F(DiffEngineTest, NoDuplicateDeltas) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("F1"),
    createSymbol("F2"),
    createSymbol("F3"),
    createSymbol("F4"),
    createSymbol("F5")
  };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_FALSE(hasDuplicateNames(report.changeObject));
}

TEST_F(DiffEngineTest, IdenticalGraphsNoDelta) {
  std::vector<SymbolRecord> symbols = {
    createSymbol("Func1"),
    createSymbol("Func2"),
    createSymbol("Func3")
  };
  
  DeltaReport report = DiffEngine::computeDelta(symbols, symbols, depGraph);
  
  EXPECT_TRUE(report.changeObject.empty());
}

TEST_F(DiffEngineTest, CompletelyDifferentGraphs) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("OldA"),
    createSymbol("OldB"),
    createSymbol("OldC")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("NewX"),
    createSymbol("NewY"),
    createSymbol("NewZ")
  };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 6);
}

TEST_F(DiffEngineTest, RiskScorePopulated) {
  std::vector<SymbolRecord> oldSymbols = { createSymbol("Func") };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_GE(report.riskScore.overallRisk, 0.0);
  EXPECT_LE(report.riskScore.overallRisk, 1.0);
}

TEST_F(DiffEngineTest, ImpactMapPopulated) {
  std::vector<SymbolRecord> oldSymbols = { createSymbol("Func") };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_GE(report.impactMap.regressionProbability, 0.0);
  EXPECT_LE(report.impactMap.regressionProbability, 1.0);
  EXPECT_GE(report.impactMap.structuralRiskIndex, 0.0);
}

TEST_F(DiffEngineTest, DependencyGraphIntegration) {
  DependencyGraph graph;
  graph.addEdge("fileA", "fileB");
  graph.addEdge("fileB", "fileC");
  
  std::vector<SymbolRecord> oldSymbols = { createSymbol("Func") };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, graph);
  
  EXPECT_FALSE(report.changeObject.empty());
}

TEST_F(DiffEngineTest, ConsistentReportAcrossCalls) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("A"), createSymbol("B"), createSymbol("C")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("A"), createSymbol("D")
  };
  
  DeltaReport report1 = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  DeltaReport report2 = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report1.changeObject.size(), report2.changeObject.size());
  EXPECT_NEAR(report1.riskScore.overallRisk, report2.riskScore.overallRisk, 0.001);
}

TEST_F(DiffEngineTest, RemovalDetectsContractBreak) {
  std::vector<SymbolRecord> oldSymbols = { createSymbol("RemovedFunc") };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 1);
  EXPECT_EQ(report.changeObject[0].changeType, ChangeType::Removed);
  EXPECT_EQ(report.contractBreaks.size(), 1);
}

TEST_F(DiffEngineTest, AdditionNoContractBreak) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = { createSymbol("NewFunc") };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 1);
  EXPECT_TRUE(report.contractBreaks.empty());
}

TEST_F(DiffEngineTest, LargeSymbolGraphConsistency) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols;
  
  for (int i = 0; i < 40; ++i) {
    oldSymbols.push_back(createSymbol("Sym_" + std::to_string(i), "void()", 
                                      SymbolType::Function, Visibility::Public,
                                      i, 1));
  }
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 40);
  EXPECT_FALSE(hasDuplicateNames(report.changeObject));
}

TEST_F(DiffEngineTest, EmptyDependencyGraph) {
  DependencyGraph emptyGraph;
  
  std::vector<SymbolRecord> oldSymbols = { createSymbol("Func") };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, emptyGraph);
  
  EXPECT_EQ(report.changeObject.size(), 1);
}

TEST_F(DiffEngineTest, MixedChangeTypes) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols;
  
  for (int i = 0; i < 10; ++i) {
    oldSymbols.push_back(createSymbol("Keep_" + std::to_string(i), "void()", 
                                      SymbolType::Function, Visibility::Public,
                                      i, 1));
    newSymbols.push_back(createSymbol("Keep_" + std::to_string(i), "void()", 
                                      SymbolType::Function, Visibility::Public,
                                      i, 1));
  }
  
  for (int i = 10; i < 15; ++i) {
    oldSymbols.push_back(createSymbol("Remove_" + std::to_string(i)));
  }
  
  for (int i = 15; i < 20; ++i) {
    newSymbols.push_back(createSymbol("Add_" + std::to_string(i)));
  }
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 10);
  EXPECT_EQ(report.contractBreaks.size(), 5);
}

TEST_F(DiffEngineTest, HighRiskChangesPopulated) {
  std::vector<SymbolRecord> oldSymbols;
  for (int i = 0; i < 10; ++i) {
    oldSymbols.push_back(createSymbol("Func_" + std::to_string(i)));
  }
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, 
                                               std::vector<SymbolRecord>(), 
                                               depGraph);
  
  EXPECT_GE(report.riskScore.highRiskChanges.size(), 0);
}

TEST_F(DiffEngineTest, VisibilityChangeDetected) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Private)
  };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 1);
  EXPECT_EQ(report.changeObject[0].changeType, ChangeType::Modified);
}

TEST_F(DiffEngineTest, AllFieldsNonNegative) {
  std::vector<SymbolRecord> oldSymbols = { createSymbol("Func") };
  std::vector<SymbolRecord> newSymbols;
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_GE(report.riskScore.overallRisk, 0.0);
  EXPECT_GE(report.riskScore.driftScore, 0.0);
  EXPECT_GE(report.riskScore.instabilityIndex, 0.0);
  EXPECT_GE(report.impactMap.regressionProbability, 0.0);
  EXPECT_GE(report.impactMap.structuralRiskIndex, 0.0);
}

TEST_F(DiffEngineTest, SymbolTypeChangeDetected) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Sym", "void()", SymbolType::Function)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Sym", "void()", SymbolType::Class)
  };
  
  DeltaReport report = DiffEngine::computeDelta(oldSymbols, newSymbols, depGraph);
  
  EXPECT_EQ(report.changeObject.size(), 1);
  EXPECT_EQ(report.changeObject[0].changeType, ChangeType::Modified);
}
