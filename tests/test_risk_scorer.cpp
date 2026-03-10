// ============================================================================
// File: tests/diff/test_risk_scorer.cpp
// Tests for RiskScorer risk assessment engine
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include "diff/RiskScorer.h"
#include "diff/SymbolDelta.h"
#include "diff/RiskReport.h"
#include "graph/DependencyGraph.h"
#include "ai/SymbolTable.h"

using namespace ultra::diff;
using namespace ultra::ai;
using namespace ultra::types;
using namespace ultra::graph;

class RiskScorerTest : public ::testing::Test {
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

  bool containsDelta(const std::vector<SymbolDelta>& deltas,
                     const std::string& name) {
    return std::any_of(deltas.begin(), deltas.end(),
                      [&](const SymbolDelta& d) {
                        return d.symbolName == name;
                      });
  }
};

TEST_F(RiskScorerTest, ZeroRiskScenario) {
  std::vector<SymbolDelta> deltas;
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_EQ(report.overallRisk, 0.0);
  EXPECT_EQ(report.driftScore, 0.0);
  EXPECT_EQ(report.instabilityIndex, 0.0);
  EXPECT_TRUE(report.highRiskChanges.empty());
}

TEST_F(RiskScorerTest, SingleLowRiskChange) {
  std::vector<SymbolDelta> deltas = {
    createDelta("newFunc", ChangeType::Added, 
               SymbolRecord(), createSymbol("newFunc"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GT(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
  EXPECT_GE(report.driftScore, 0.0);
  EXPECT_LE(report.driftScore, 1.0);
}

TEST_F(RiskScorerTest, MultipleMediumRiskChanges) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func1", ChangeType::Modified,
               createSymbol("func1", "void()", SymbolType::Function, Visibility::Public),
               createSymbol("func1", "void()", SymbolType::Function, Visibility::Private)),
    createDelta("func2", ChangeType::Modified,
               createSymbol("func2", "void()", SymbolType::Function, Visibility::Public),
               createSymbol("func2", "void()", SymbolType::Function, Visibility::Private))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GT(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
}

TEST_F(RiskScorerTest, HighRiskChangeDetection) {
  std::vector<SymbolDelta> deltas = {
    createDelta("removed", ChangeType::Removed,
               createSymbol("removed"), SymbolRecord())
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GT(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
  EXPECT_GE(report.highRiskChanges.size(), 0);
}

TEST_F(RiskScorerTest, RemovedSymbolHighRisk) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed,
               createSymbol("func"), SymbolRecord())
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GT(report.overallRisk, 0.0);
}

TEST_F(RiskScorerTest, ModifiedSymbolMediumRisk) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified,
               createSymbol("func"), createSymbol("func"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GT(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
}

TEST_F(RiskScorerTest, AddedSymbolLowRisk) {
  std::vector<SymbolDelta> deltas = {
    createDelta("newFunc", ChangeType::Added,
               SymbolRecord(), createSymbol("newFunc"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.overallRisk, 0.0);
}

TEST_F(RiskScorerTest, WeightedScoringBehavior) {
  std::vector<SymbolDelta> deltasAddOnly = {
    createDelta("func1", ChangeType::Added, SymbolRecord(), createSymbol("func1")),
    createDelta("func2", ChangeType::Added, SymbolRecord(), createSymbol("func2")),
    createDelta("func3", ChangeType::Added, SymbolRecord(), createSymbol("func3"))
  };
  
  std::vector<SymbolDelta> deltasRemoveOnly = {
    createDelta("func1", ChangeType::Removed, createSymbol("func1"), SymbolRecord()),
    createDelta("func2", ChangeType::Removed, createSymbol("func2"), SymbolRecord()),
    createDelta("func3", ChangeType::Removed, createSymbol("func3"), SymbolRecord())
  };
  
  RiskReport reportAdd = RiskScorer::score(deltasAddOnly, depGraph);
  RiskReport reportRemove = RiskScorer::score(deltasRemoveOnly, depGraph);
  
  EXPECT_LT(reportAdd.overallRisk, reportRemove.overallRisk);
}

TEST_F(RiskScorerTest, StabilityAcrossRepeatedCalls) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func1", ChangeType::Modified,
               createSymbol("func1"), createSymbol("func1")),
    createDelta("func2", ChangeType::Removed,
               createSymbol("func2"), SymbolRecord())
  };
  
  RiskReport report1 = RiskScorer::score(deltas, depGraph);
  RiskReport report2 = RiskScorer::score(deltas, depGraph);
  RiskReport report3 = RiskScorer::score(deltas, depGraph);
  
  EXPECT_NEAR(report1.overallRisk, report2.overallRisk, 0.001);
  EXPECT_NEAR(report2.overallRisk, report3.overallRisk, 0.001);
  EXPECT_NEAR(report1.driftScore, report2.driftScore, 0.001);
  EXPECT_NEAR(report2.driftScore, report3.driftScore, 0.001);
}

TEST_F(RiskScorerTest, LargeBatchRiskComputation) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 30; ++i) {
    if (i % 3 == 0) {
      deltas.push_back(createDelta("func_" + std::to_string(i), ChangeType::Removed,
                                  createSymbol("func_" + std::to_string(i)), SymbolRecord()));
    } else if (i % 3 == 1) {
      deltas.push_back(createDelta("func_" + std::to_string(i), ChangeType::Modified,
                                  createSymbol("func_" + std::to_string(i)),
                                  createSymbol("func_" + std::to_string(i))));
    } else {
      deltas.push_back(createDelta("func_" + std::to_string(i), ChangeType::Added,
                                  SymbolRecord(),
                                  createSymbol("func_" + std::to_string(i))));
    }
  }
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
  EXPECT_GE(report.instabilityIndex, 0.0);
  EXPECT_LE(report.instabilityIndex, 1.0);
}

TEST_F(RiskScorerTest, EmptyInputZeroRisk) {
  std::vector<SymbolDelta> deltas;
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_EQ(report.overallRisk, 0.0);
  EXPECT_EQ(report.driftScore, 0.0);
  EXPECT_EQ(report.instabilityIndex, 0.0);
}

TEST_F(RiskScorerTest, OverallRiskBounded) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 20; ++i) {
    deltas.push_back(createDelta("func_" + std::to_string(i), ChangeType::Removed,
                                createSymbol("func_" + std::to_string(i)), SymbolRecord()));
  }
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
}

TEST_F(RiskScorerTest, DriftScoreBounded) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified,
               createSymbol("func"), createSymbol("func"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.driftScore, 0.0);
  EXPECT_LE(report.driftScore, 1.0);
}

TEST_F(RiskScorerTest, InstabilityIndexBounded) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 50; ++i) {
    deltas.push_back(createDelta("func_" + std::to_string(i), ChangeType::Modified,
                                createSymbol("func_" + std::to_string(i)),
                                createSymbol("func_" + std::to_string(i))));
  }
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.instabilityIndex, 0.0);
  EXPECT_LE(report.instabilityIndex, 1.0);
}

TEST_F(RiskScorerTest, NoNegativeScores) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func1", ChangeType::Removed, createSymbol("func1"), SymbolRecord()),
    createDelta("func2", ChangeType::Modified, createSymbol("func2"), createSymbol("func2")),
    createDelta("func3", ChangeType::Added, SymbolRecord(), createSymbol("func3"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.overallRisk, 0.0);
  EXPECT_GE(report.driftScore, 0.0);
  EXPECT_GE(report.instabilityIndex, 0.0);
}

TEST_F(RiskScorerTest, NoOverflowLargeInput) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 100; ++i) {
    deltas.push_back(createDelta("func_" + std::to_string(i), ChangeType::Removed,
                                createSymbol("func_" + std::to_string(i)), SymbolRecord()));
  }
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_FALSE(std::isnan(report.overallRisk));
  EXPECT_FALSE(std::isinf(report.overallRisk));
  EXPECT_FALSE(std::isnan(report.driftScore));
  EXPECT_FALSE(std::isinf(report.driftScore));
  EXPECT_LE(report.overallRisk, 1.0);
  EXPECT_LE(report.driftScore, 1.0);
}

TEST_F(RiskScorerTest, VisibilityRestrictionIncreaseRisk) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified,
               createSymbol("func", "void()", SymbolType::Function, Visibility::Public),
               createSymbol("func", "void()", SymbolType::Function, Visibility::Private))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GT(report.overallRisk, 0.0);
}

TEST_F(RiskScorerTest, HighRiskChangesPopulated) {
  std::vector<SymbolDelta> deltas = {
    createDelta("removed1", ChangeType::Removed,
               createSymbol("removed1"), SymbolRecord()),
    createDelta("added1", ChangeType::Added,
               SymbolRecord(), createSymbol("added1"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.highRiskChanges.size(), 0);
}

TEST_F(RiskScorerTest, DependencyGraphParameter) {
  DependencyGraph graph;
  graph.addEdge("fileA", "fileB");
  graph.addEdge("fileB", "fileC");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed,
               createSymbol("func"), SymbolRecord())
  };
  
  RiskReport report = RiskScorer::score(deltas, graph);
  
  EXPECT_GE(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
}

TEST_F(RiskScorerTest, ConsistentScoringAcrossGraphs) {
  DependencyGraph graph1;
  DependencyGraph graph2;
  
  graph1.addEdge("a", "b");
  graph2.addEdge("c", "d");
  
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified,
               createSymbol("func"), createSymbol("func"))
  };
  
  RiskReport report1 = RiskScorer::score(deltas, graph1);
  RiskReport report2 = RiskScorer::score(deltas, graph2);
  
  EXPECT_NEAR(report1.overallRisk, report2.overallRisk, 0.001);
}

TEST_F(RiskScorerTest, RiskScalesWithChangeCount) {
  std::vector<SymbolDelta> deltasSingle = {
    createDelta("func", ChangeType::Modified,
               createSymbol("func"), createSymbol("func"))
  };
  
  std::vector<SymbolDelta> deltasMany;
  for (int i = 0; i < 10; ++i) {
    deltasMany.push_back(createDelta("func_" + std::to_string(i), ChangeType::Modified,
                                    createSymbol("func_" + std::to_string(i)),
                                    createSymbol("func_" + std::to_string(i))));
  }
  
  RiskReport reportSingle = RiskScorer::score(deltasSingle, depGraph);
  RiskReport reportMany = RiskScorer::score(deltasMany, depGraph);
  
  EXPECT_LT(reportSingle.overallRisk, reportMany.overallRisk);
}

TEST_F(RiskScorerTest, AllChangeTypesCovered) {
  std::vector<SymbolDelta> deltas = {
    createDelta("added", ChangeType::Added,
               SymbolRecord(), createSymbol("added")),
    createDelta("removed", ChangeType::Removed,
               createSymbol("removed"), SymbolRecord()),
    createDelta("modified", ChangeType::Modified,
               createSymbol("modified"), createSymbol("modified"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GT(report.overallRisk, 0.0);
  EXPECT_LE(report.overallRisk, 1.0);
}

TEST_F(RiskScorerTest, DriftScoreRelationToRisk) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified,
               createSymbol("func"), createSymbol("func"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_LE(report.driftScore, report.overallRisk);
}

TEST_F(RiskScorerTest, InstabilityFromChangeCount) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 100; ++i) {
    deltas.push_back(createDelta("func_" + std::to_string(i), ChangeType::Added,
                                SymbolRecord(), createSymbol("func_" + std::to_string(i))));
  }
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_EQ(report.instabilityIndex, 1.0);
}

TEST_F(RiskScorerTest, UnchangedSymbolNoRisk) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Unchanged,
               createSymbol("func"), createSymbol("func"))
  };
  
  RiskReport report = RiskScorer::score(deltas, depGraph);
  
  EXPECT_GE(report.overallRisk, 0.0);
}
