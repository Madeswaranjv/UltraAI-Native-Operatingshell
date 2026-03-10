// ============================================================================
// File: tests/diff/test_signature_diff.cpp
// Tests for SignatureDiff contract break detection engine
// ============================================================================

#include <gtest/gtest.h>
#include <algorithm>
#include "diff/SignatureDiff.h"
#include "diff/SymbolDelta.h"
#include "graph/DependencyGraph.h"
#include "ai/SymbolTable.h"

using namespace ultra::diff;
using namespace ultra::ai;
using namespace ultra::types;
using namespace ultra::graph;

class SignatureDiffTest : public ::testing::Test {
 protected:
  DependencyGraph depGraph;

  SymbolRecord createSymbol(const std::string& name,
                           const std::string& signature = "void()",
                           SymbolType symbolType = SymbolType::Function,
                           Visibility visibility = Visibility::Public) {
    SymbolRecord rec;
    rec.name = name;
    rec.signature = signature;
    rec.symbolType = symbolType;
    rec.visibility = visibility;
    rec.symbolId = 1;
    rec.fileId = 1;
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

  bool containsBreak(const std::vector<ContractBreak>& breaks,
                     const std::string& functionName,
                     BreakType breakType) {
    return std::any_of(breaks.begin(), breaks.end(),
                      [&](const ContractBreak& b) {
                        return b.functionName == functionName &&
                               b.breakType == breakType;
                      });
  }
};

TEST_F(SignatureDiffTest, EmptyDeltasReturnsEmpty) {
  std::vector<SymbolDelta> deltas;
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_TRUE(breaks.empty());
}

TEST_F(SignatureDiffTest, DetectRemovedSymbol) {
  SymbolRecord oldRec = createSymbol("removedFunc");
  std::vector<SymbolDelta> deltas = {
    createDelta("removedFunc", ChangeType::Removed, oldRec, SymbolRecord())
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_EQ(breaks.size(), 1);
  EXPECT_EQ(breaks[0].functionName, "removedFunc");
  EXPECT_EQ(breaks[0].breakType, BreakType::Removed);
  EXPECT_EQ(breaks[0].severity, 1.0);
}

TEST_F(SignatureDiffTest, DetectVisibilityChange) {
  SymbolRecord oldRec = createSymbol("restrictedFunc", "void()", 
                                     SymbolType::Function, Visibility::Public);
  SymbolRecord newRec = createSymbol("restrictedFunc", "void()", 
                                     SymbolType::Function, Visibility::Private);
  std::vector<SymbolDelta> deltas = {
    createDelta("restrictedFunc", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_TRUE(std::any_of(breaks.begin(), breaks.end(),
                         [](const ContractBreak& b) {
                           return b.breakType == BreakType::VisibilityChange;
                         }));
}

TEST_F(SignatureDiffTest, DetectSignatureChange) {
  SymbolRecord oldRec = createSymbol("changedFunc", "void()");
  SymbolRecord newRec = createSymbol("changedFunc", "int(std::string)");
  std::vector<SymbolDelta> deltas = {
    createDelta("changedFunc", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_TRUE(std::any_of(breaks.begin(), breaks.end(),
                         [](const ContractBreak& b) {
                           return b.breakType == BreakType::ParameterChange;
                         }));
}

TEST_F(SignatureDiffTest, DetectTypeChange) {
  SymbolRecord oldRec = createSymbol("changedType", "void()", SymbolType::Function);
  SymbolRecord newRec = createSymbol("changedType", "void()", SymbolType::Class);
  std::vector<SymbolDelta> deltas = {
    createDelta("changedType", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_TRUE(std::any_of(breaks.begin(), breaks.end(),
                         [](const ContractBreak& b) {
                           return b.breakType == BreakType::TypeChange;
                         }));
}

TEST_F(SignatureDiffTest, NoBreakForAddedSymbol) {
  SymbolRecord newRec = createSymbol("newFunc");
  std::vector<SymbolDelta> deltas = {
    createDelta("newFunc", ChangeType::Added, SymbolRecord(), newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_TRUE(breaks.empty());
}

TEST_F(SignatureDiffTest, NoBreakForUnchangedSymbol) {
  SymbolRecord oldRec = createSymbol("unchanged");
  SymbolRecord newRec = createSymbol("unchanged");
  std::vector<SymbolDelta> deltas = {
    createDelta("unchanged", ChangeType::Unchanged, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_TRUE(breaks.empty());
}

TEST_F(SignatureDiffTest, MultipleRemovals) {
  std::vector<SymbolDelta> deltas = {
    createDelta("func1", ChangeType::Removed, createSymbol("func1"), SymbolRecord()),
    createDelta("func2", ChangeType::Removed, createSymbol("func2"), SymbolRecord()),
    createDelta("func3", ChangeType::Removed, createSymbol("func3"), SymbolRecord())
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks.size(), 3);
}

TEST_F(SignatureDiffTest, MultipleModifications) {
  std::vector<SymbolDelta> deltas;
  
  SymbolRecord oldRec1 = createSymbol("func1", "void()", SymbolType::Function, Visibility::Public);
  SymbolRecord newRec1 = createSymbol("func1", "void()", SymbolType::Function, Visibility::Private);
  deltas.push_back(createDelta("func1", ChangeType::Modified, oldRec1, newRec1));
  
  SymbolRecord oldRec2 = createSymbol("func2", "int()");
  SymbolRecord newRec2 = createSymbol("func2", "int(std::string)");
  deltas.push_back(createDelta("func2", ChangeType::Modified, oldRec2, newRec2));
  
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks.size(), 2);
}

TEST_F(SignatureDiffTest, MixedChanges) {
  std::vector<SymbolDelta> deltas;
  
  deltas.push_back(createDelta("removed", ChangeType::Removed, 
                              createSymbol("removed"), SymbolRecord()));
  
  deltas.push_back(createDelta("added", ChangeType::Added, 
                              SymbolRecord(), createSymbol("added")));
  
  SymbolRecord oldRec = createSymbol("modified", "void()", SymbolType::Function, Visibility::Public);
  SymbolRecord newRec = createSymbol("modified", "void()", SymbolType::Function, Visibility::Private);
  deltas.push_back(createDelta("modified", ChangeType::Modified, oldRec, newRec));
  
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks.size(), 2);
  EXPECT_TRUE(containsBreak(breaks, "removed", BreakType::Removed));
  EXPECT_TRUE(containsBreak(breaks, "modified", BreakType::VisibilityChange));
}

TEST_F(SignatureDiffTest, LargeSymbolSet20Functions) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 20; ++i) {
    std::string name = "func_" + std::to_string(i);
    deltas.push_back(createDelta(name, ChangeType::Removed, 
                                createSymbol(name), SymbolRecord()));
  }
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks.size(), 20);
}

TEST_F(SignatureDiffTest, DeterministicFirstRun) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 10; ++i) {
    std::string name = "func_" + std::to_string(i);
    deltas.push_back(createDelta(name, ChangeType::Removed, 
                                createSymbol(name), SymbolRecord()));
  }
  auto breaks1 = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks1.size(), 10);
}

TEST_F(SignatureDiffTest, DeterministicSecondRun) {
  std::vector<SymbolDelta> deltas;
  for (int i = 0; i < 10; ++i) {
    std::string name = "func_" + std::to_string(i);
    deltas.push_back(createDelta(name, ChangeType::Removed, 
                                createSymbol(name), SymbolRecord()));
  }
  auto breaks2 = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks2.size(), 10);
}

TEST_F(SignatureDiffTest, VisibilityChangeCorrectSeverity) {
  SymbolRecord oldRec = createSymbol("func", "void()", SymbolType::Function, Visibility::Public);
  SymbolRecord newRec = createSymbol("func", "void()", SymbolType::Function, Visibility::Private);
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_EQ(breaks.size(), 1);
  EXPECT_EQ(breaks[0].severity, 0.9);
}

TEST_F(SignatureDiffTest, SignatureChangeCorrectSeverity) {
  SymbolRecord oldRec = createSymbol("func", "void()");
  SymbolRecord newRec = createSymbol("func", "int(std::string)");
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_TRUE(std::any_of(breaks.begin(), breaks.end(),
                         [](const ContractBreak& b) {
                           return b.breakType == BreakType::ParameterChange;
                         }));
}

TEST_F(SignatureDiffTest, RemovedFunctionHasDescription) {
  SymbolRecord oldRec = createSymbol("removed");
  std::vector<SymbolDelta> deltas = {
    createDelta("removed", ChangeType::Removed, oldRec, SymbolRecord())
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_EQ(breaks.size(), 1);
  EXPECT_FALSE(breaks[0].description.empty());
  EXPECT_TRUE(breaks[0].description.find("removed") != std::string::npos);
}

TEST_F(SignatureDiffTest, VisibilityRestrictionFromPublic) {
  SymbolRecord oldRec = createSymbol("func", "void()", SymbolType::Function, Visibility::Public);
  SymbolRecord newRec = createSymbol("func", "void()", SymbolType::Function, Visibility::Private);
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  ASSERT_EQ(breaks.size(), 1);
  EXPECT_EQ(breaks[0].breakType, BreakType::VisibilityChange);
}

TEST_F(SignatureDiffTest, VisibilityExpansionNotBreaking) {
  SymbolRecord oldRec = createSymbol("func", "void()", SymbolType::Function, Visibility::Private);
  SymbolRecord newRec = createSymbol("func", "void()", SymbolType::Function, Visibility::Public);
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_TRUE(breaks.empty());
}

TEST_F(SignatureDiffTest, DependencyGraphParameter) {
  DependencyGraph graph;
  graph.addEdge("fileA", "fileB");
  graph.addEdge("fileB", "fileC");
  
  SymbolRecord oldRec = createSymbol("func");
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Removed, oldRec, SymbolRecord())
  };
  
  auto breaks = SignatureDiff::analyze(deltas, graph);
  EXPECT_EQ(breaks.size(), 1);
}

TEST_F(SignatureDiffTest, AllBreakTypesDetectable) {
  std::vector<SymbolDelta> deltas;
  
  deltas.push_back(createDelta("removed", ChangeType::Removed, 
                              createSymbol("removed"), SymbolRecord()));
  
  SymbolRecord oldRec1 = createSymbol("visChanged", "void()", 
                                      SymbolType::Function, Visibility::Public);
  SymbolRecord newRec1 = createSymbol("visChanged", "void()", 
                                      SymbolType::Function, Visibility::Private);
  deltas.push_back(createDelta("visChanged", ChangeType::Modified, oldRec1, newRec1));
  
  SymbolRecord oldRec2 = createSymbol("sigChanged", "void()");
  SymbolRecord newRec2 = createSymbol("sigChanged", "int()");
  deltas.push_back(createDelta("sigChanged", ChangeType::Modified, oldRec2, newRec2));
  
  SymbolRecord oldRec3 = createSymbol("typeChanged", "void()", SymbolType::Function);
  SymbolRecord newRec3 = createSymbol("typeChanged", "void()", SymbolType::Class);
  deltas.push_back(createDelta("typeChanged", ChangeType::Modified, oldRec3, newRec3));
  
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks.size(), 4);
}

TEST_F(SignatureDiffTest, NoBreakForModificationWithoutChanges) {
  SymbolRecord oldRec = createSymbol("unchanged", "void()", 
                                     SymbolType::Function, Visibility::Public);
  SymbolRecord newRec = createSymbol("unchanged", "void()", 
                                     SymbolType::Function, Visibility::Public);
  std::vector<SymbolDelta> deltas = {
    createDelta("unchanged", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_TRUE(breaks.empty());
}

TEST_F(SignatureDiffTest, SpecialCharactersInFunctionName) {
  std::vector<SymbolDelta> deltas = {
    createDelta("operator+", ChangeType::Removed, createSymbol("operator+"), SymbolRecord()),
    createDelta("MyClass::method", ChangeType::Removed, createSymbol("MyClass::method"), SymbolRecord())
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks.size(), 2);
}

TEST_F(SignatureDiffTest, LargeSignatures) {
  std::string largeSig1(500, 'a');
  std::string largeSig2(500, 'b');
  
  SymbolRecord oldRec = createSymbol("func", largeSig1);
  SymbolRecord newRec = createSymbol("func", largeSig2);
  std::vector<SymbolDelta> deltas = {
    createDelta("func", ChangeType::Modified, oldRec, newRec)
  };
  auto breaks = SignatureDiff::analyze(deltas, depGraph);
  EXPECT_EQ(breaks.size(), 1);
}
