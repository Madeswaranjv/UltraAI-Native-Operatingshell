//test_symbol_diff.cpp
#include <gtest/gtest.h>
#include <algorithm>
#include "diff/SymbolDiff.h"
#include "ai/SymbolTable.h"

using namespace ultra::diff;
using namespace ultra::ai;
using namespace ultra::types;

class SymbolDiffTest : public ::testing::Test {
 protected:
  SymbolRecord createSymbol(const std::string& name,
                           const std::string& signature = "void()",
                           SymbolType symbolType = SymbolType::Function,
                           Visibility visibility = Visibility::Public,
                           uint64_t symbolId = 1,
                           uint32_t fileId = 1,
                           uint32_t lineNumber = 1) {
    SymbolRecord rec;
    rec.symbolId = symbolId;
    rec.fileId = fileId;
    rec.name = name;
    rec.signature = signature;
    rec.symbolType = symbolType;
    rec.visibility = visibility;
    rec.lineNumber = lineNumber;
    return rec;
  }

  bool containsDelta(const std::vector<SymbolDelta>& deltas,
                     const std::string& symbolName,
                     ChangeType changeType) {
    return std::any_of(deltas.begin(), deltas.end(),
                      [&](const SymbolDelta& d) {
                        return d.symbolName == symbolName &&
                               d.changeType == changeType;
                      });
  }

  bool containsSymbolName(const std::vector<SymbolDelta>& deltas,
                          const std::string& symbolName) {
    return std::any_of(deltas.begin(), deltas.end(),
                      [&](const SymbolDelta& d) {
                        return d.symbolName == symbolName;
                      });
  }

  SymbolDelta* findDelta(std::vector<SymbolDelta>& deltas,
                         const std::string& symbolName) {
    auto it = std::find_if(deltas.begin(), deltas.end(),
                          [&](const SymbolDelta& d) {
                            return d.symbolName == symbolName;
                          });
    return it != deltas.end() ? &(*it) : nullptr;
  }
};

TEST_F(SymbolDiffTest, EmptyOldEmptyNewReturnsEmpty) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols;
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_TRUE(deltas.empty());
}

TEST_F(SymbolDiffTest, DetectAddedSymbol) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("NewFunction")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].symbolName, "NewFunction");
  EXPECT_EQ(deltas[0].changeType, ChangeType::Added);
}

TEST_F(SymbolDiffTest, DetectRemovedSymbol) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("RemovedFunction")
  };
  std::vector<SymbolRecord> newSymbols;
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].symbolName, "RemovedFunction");
  EXPECT_EQ(deltas[0].changeType, ChangeType::Removed);
}

TEST_F(SymbolDiffTest, DetectUnchangedSymbol) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("UnchangedFunc", "void()")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("UnchangedFunc", "void()")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_TRUE(deltas.empty());
}

TEST_F(SymbolDiffTest, DetectModifiedSymbolType) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("MySymbol", "void()", SymbolType::Function)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("MySymbol", "void()", SymbolType::Class)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_TRUE(containsDelta(deltas, "MySymbol", ChangeType::Modified));
}

TEST_F(SymbolDiffTest, DetectModifiedVisibility) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("MySymbol", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("MySymbol", "void()", SymbolType::Function, Visibility::Private)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_TRUE(containsDelta(deltas, "MySymbol", ChangeType::Modified));
}

TEST_F(SymbolDiffTest, ModifiedSymbolContainsOldAndNewRecords) {
  SymbolRecord oldRec = createSymbol("Func", "int()", SymbolType::Function, Visibility::Public);
  SymbolRecord newRec = createSymbol("Func", "int()", SymbolType::Function, Visibility::Private);
  
  std::vector<SymbolRecord> oldSymbols = { oldRec };
  std::vector<SymbolRecord> newSymbols = { newRec };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].oldRecord.visibility, Visibility::Public);
  EXPECT_EQ(deltas[0].newRecord.visibility, Visibility::Private);
}

TEST_F(SymbolDiffTest, AddedSymbolContainsNewRecord) {
  SymbolRecord newRec = createSymbol("Added", "void()");
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = { newRec };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].changeType, ChangeType::Added);
  EXPECT_EQ(deltas[0].newRecord.name, "Added");
}

TEST_F(SymbolDiffTest, RemovedSymbolContainsOldRecord) {
  SymbolRecord oldRec = createSymbol("Removed", "void()");
  std::vector<SymbolRecord> oldSymbols = { oldRec };
  std::vector<SymbolRecord> newSymbols;
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].changeType, ChangeType::Removed);
  EXPECT_EQ(deltas[0].oldRecord.name, "Removed");
}

TEST_F(SymbolDiffTest, MultipleAdditions) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func1"),
    createSymbol("Func2"),
    createSymbol("Func3")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 3);
  EXPECT_TRUE(containsDelta(deltas, "Func1", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "Func2", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "Func3", ChangeType::Added));
}

TEST_F(SymbolDiffTest, MultipleRemovals) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Old1"),
    createSymbol("Old2"),
    createSymbol("Old3")
  };
  std::vector<SymbolRecord> newSymbols;
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 3);
  EXPECT_TRUE(containsDelta(deltas, "Old1", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(deltas, "Old2", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(deltas, "Old3", ChangeType::Removed));
}

TEST_F(SymbolDiffTest, MultipleModifications) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Func1", "void()", SymbolType::Function, Visibility::Public),
    createSymbol("Func2", "void()", SymbolType::Function, Visibility::Public),
    createSymbol("Func3", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func1", "void()", SymbolType::Function, Visibility::Private),
    createSymbol("Func2", "void()", SymbolType::Class, Visibility::Public),
    createSymbol("Func3", "void()", SymbolType::Function, Visibility::Protected)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 3);
  EXPECT_TRUE(containsDelta(deltas, "Func1", ChangeType::Modified));
  EXPECT_TRUE(containsDelta(deltas, "Func2", ChangeType::Modified));
  EXPECT_TRUE(containsDelta(deltas, "Func3", ChangeType::Modified));
}

TEST_F(SymbolDiffTest, MixedChanges) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Unchanged"),
    createSymbol("ToRemove"),
    createSymbol("ToModify", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Unchanged"),
    createSymbol("ToAdd"),
    createSymbol("ToModify", "void()", SymbolType::Function, Visibility::Private)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 3);
  EXPECT_TRUE(containsDelta(deltas, "ToAdd", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "ToRemove", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(deltas, "ToModify", ChangeType::Modified));
}

TEST_F(SymbolDiffTest, LargeSymbolSet20Symbols) {
  std::vector<SymbolRecord> oldSymbols;
  for (int i = 0; i < 20; ++i) {
    oldSymbols.push_back(createSymbol("Old_" + std::to_string(i)));
  }
  auto deltas = SymbolDiff::compare(oldSymbols, std::vector<SymbolRecord>());
  EXPECT_EQ(deltas.size(), 20);
}

TEST_F(SymbolDiffTest, LargeSymbolSet30Additions) {
  std::vector<SymbolRecord> newSymbols;
  for (int i = 0; i < 30; ++i) {
    newSymbols.push_back(createSymbol("New_" + std::to_string(i)));
  }
  auto deltas = SymbolDiff::compare(std::vector<SymbolRecord>(), newSymbols);
  EXPECT_EQ(deltas.size(), 30);
}

TEST_F(SymbolDiffTest, LargeSymbolSetMixedChanges) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols;
  
  for (int i = 0; i < 10; ++i) {
    oldSymbols.push_back(createSymbol("Unchanged_" + std::to_string(i)));
    newSymbols.push_back(createSymbol("Unchanged_" + std::to_string(i)));
  }
  
  for (int i = 0; i < 10; ++i) {
    oldSymbols.push_back(createSymbol("Removed_" + std::to_string(i)));
  }
  
  for (int i = 0; i < 10; ++i) {
    newSymbols.push_back(createSymbol("Added_" + std::to_string(i)));
  }
  
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_EQ(deltas.size(), 20);
}

TEST_F(SymbolDiffTest, DuplicateSymbolInOld) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Duplicate"),
    createSymbol("Duplicate")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Duplicate")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].changeType, ChangeType::Removed);
}

TEST_F(SymbolDiffTest, SpecialCharactersInSymbolName) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("func::with::scope"),
    createSymbol("operator+"),
    createSymbol("_underscore_name"),
    createSymbol("name-with-dash")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 4);
  EXPECT_TRUE(containsDelta(deltas, "func::with::scope", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "operator+", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "_underscore_name", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "name-with-dash", ChangeType::Added));
}

TEST_F(SymbolDiffTest, LongSymbolNames) {
  std::string longName(500, 'a');
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol(longName)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].symbolName, longName);
}

TEST_F(SymbolDiffTest, EmptyStringSymbolName) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("")
  };
  std::vector<SymbolRecord> newSymbols;
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].symbolName, "");
  EXPECT_EQ(deltas[0].changeType, ChangeType::Removed);
}

TEST_F(SymbolDiffTest, VariousSymbolTypes) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Class", "class()", SymbolType::Class),
    createSymbol("Function", "void()", SymbolType::Function),
    createSymbol("EntryPoint", "int()", SymbolType::EntryPoint),
    createSymbol("Import", "import()", SymbolType::Import),
    createSymbol("Export", "export()", SymbolType::Export),
    createSymbol("Component", "component()", SymbolType::ReactComponent)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_EQ(deltas.size(), 6);
}

TEST_F(SymbolDiffTest, VariousVisibilities) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Public", "void()", SymbolType::Function, Visibility::Public),
    createSymbol("Private", "void()", SymbolType::Function, Visibility::Private),
    createSymbol("Protected", "void()", SymbolType::Function, Visibility::Protected),
    createSymbol("Module", "void()", SymbolType::Function, Visibility::Module),
    createSymbol("Unknown", "void()", SymbolType::Function, Visibility::Unknown)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_EQ(deltas.size(), 5);
}

TEST_F(SymbolDiffTest, DeterministicFirstRun) {
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
  auto deltas1 = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_EQ(deltas1.size(), 4);
}

TEST_F(SymbolDiffTest, DeterministicSecondRun) {
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
  auto deltas2 = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_EQ(deltas2.size(), 4);
}

TEST_F(SymbolDiffTest, DeterministicConsistency) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("X"),
    createSymbol("Y"),
    createSymbol("Z")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("X"),
    createSymbol("A"),
    createSymbol("B")
  };
  
  auto deltas1 = SymbolDiff::compare(oldSymbols, newSymbols);
  auto deltas2 = SymbolDiff::compare(oldSymbols, newSymbols);
  
  ASSERT_EQ(deltas1.size(), deltas2.size());
  for (size_t i = 0; i < deltas1.size(); ++i) {
    EXPECT_EQ(deltas1[i].symbolName, deltas2[i].symbolName);
    EXPECT_EQ(deltas1[i].changeType, deltas2[i].changeType);
  }
}

TEST_F(SymbolDiffTest, AllSymbolsAdded) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("F1"), createSymbol("F2"), createSymbol("F3"),
    createSymbol("F4"), createSymbol("F5")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 5);
  for (const auto& d : deltas) {
    EXPECT_EQ(d.changeType, ChangeType::Added);
  }
}

TEST_F(SymbolDiffTest, AllSymbolsRemoved) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("F1"), createSymbol("F2"), createSymbol("F3"),
    createSymbol("F4"), createSymbol("F5")
  };
  std::vector<SymbolRecord> newSymbols;
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 5);
  for (const auto& d : deltas) {
    EXPECT_EQ(d.changeType, ChangeType::Removed);
  }
}

TEST_F(SymbolDiffTest, AllSymbolsModified) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("F", "void()", SymbolType::Function, Visibility::Public),
    createSymbol("G", "void()", SymbolType::Function, Visibility::Public),
    createSymbol("H", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("F", "void()", SymbolType::Function, Visibility::Private),
    createSymbol("G", "void()", SymbolType::Class, Visibility::Public),
    createSymbol("H", "void()", SymbolType::Function, Visibility::Protected)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 3);
  for (const auto& d : deltas) {
    EXPECT_EQ(d.changeType, ChangeType::Modified);
  }
}

TEST_F(SymbolDiffTest, SignatureChangeTriggersModification) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Func", "void()")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func", "int()")
  };

  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);

  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].symbolName, "Func");
  EXPECT_EQ(deltas[0].changeType, ChangeType::Modified);
  EXPECT_EQ(deltas[0].oldRecord.signature, "void()");
  EXPECT_EQ(deltas[0].newRecord.signature, "int()");
}

TEST_F(SymbolDiffTest, BothVisibilityAndTypeChange) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Sym", "void()", SymbolType::Function, Visibility::Public)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Sym", "void()", SymbolType::Class, Visibility::Private)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].changeType, ChangeType::Modified);
}

TEST_F(SymbolDiffTest, LineNumberDoesNotTriggerModification) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public, 1, 1, 10)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public, 1, 1, 20)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_TRUE(deltas.empty());
}

TEST_F(SymbolDiffTest, FileIdDoesNotTriggerModification) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public, 1, 1)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public, 1, 2)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_TRUE(deltas.empty());
}

TEST_F(SymbolDiffTest, SymbolIdDoesNotTriggerModification) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public, 100)
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func", "void()", SymbolType::Function, Visibility::Public, 200)
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_TRUE(deltas.empty());
}

TEST_F(SymbolDiffTest, ContainmentCheck) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Keep1"),
    createSymbol("Keep2"),
    createSymbol("Remove1")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Keep1"),
    createSymbol("Keep2"),
    createSymbol("Add1")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_TRUE(containsSymbolName(deltas, "Remove1"));
  EXPECT_TRUE(containsSymbolName(deltas, "Add1"));
  EXPECT_FALSE(containsSymbolName(deltas, "Keep1"));
  EXPECT_FALSE(containsSymbolName(deltas, "Keep2"));
}

TEST_F(SymbolDiffTest, CaseSensitiveNameComparison) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("func")
  };
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Func")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  EXPECT_EQ(deltas.size(), 2);
  EXPECT_TRUE(containsDelta(deltas, "func", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(deltas, "Func", ChangeType::Added));
}

TEST_F(SymbolDiffTest, DefaultDeltaFields) {
  std::vector<SymbolRecord> oldSymbols;
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Sym")
  };
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  ASSERT_EQ(deltas.size(), 1);
  EXPECT_EQ(deltas[0].confidence, 1.0);
}

TEST_F(SymbolDiffTest, ComplexScenario) {
  std::vector<SymbolRecord> oldSymbols = {
    createSymbol("Maintain1", "void()", SymbolType::Function, Visibility::Public),
    createSymbol("Maintain2", "void()", SymbolType::Class, Visibility::Private),
    createSymbol("ToRemove1"),
    createSymbol("ToRemove2"),
    createSymbol("ToModify", "old()", SymbolType::Function, Visibility::Public)
  };
  
  std::vector<SymbolRecord> newSymbols = {
    createSymbol("Maintain1", "void()", SymbolType::Function, Visibility::Public),
    createSymbol("Maintain2", "void()", SymbolType::Class, Visibility::Private),
    createSymbol("ToAdd1"),
    createSymbol("ToAdd2"),
    createSymbol("ToModify", "old()", SymbolType::Function, Visibility::Protected)
  };
  
  auto deltas = SymbolDiff::compare(oldSymbols, newSymbols);
  
  EXPECT_EQ(deltas.size(), 5);
  EXPECT_TRUE(containsDelta(deltas, "ToRemove1", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(deltas, "ToRemove2", ChangeType::Removed));
  EXPECT_TRUE(containsDelta(deltas, "ToAdd1", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "ToAdd2", ChangeType::Added));
  EXPECT_TRUE(containsDelta(deltas, "ToModify", ChangeType::Modified));
}
