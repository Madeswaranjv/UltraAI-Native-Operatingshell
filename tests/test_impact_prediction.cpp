#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "engine/impact/ImpactGraphTraversal.h"
#include "engine/impact/ImpactPredictionEngine.h"
//E:\Projects\Ultra\tests\test_impact_prediction.cpp
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
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

ultra::ai::RuntimeState makeImpactState(const bool shuffled) {
  ultra::ai::RuntimeState state;

  ultra::ai::FileRecord core;
  core.fileId = 1U;
  core.path = "core.cpp";
  ultra::ai::FileRecord service;
  service.fileId = 2U;
  service.path = "service.cpp";
  ultra::ai::FileRecord app;
  app.fileId = 3U;
  app.path = "app.cpp";
  ultra::ai::FileRecord worker;
  worker.fileId = 4U;
  worker.path = "worker.cpp";
  state.files = {core, service, app, worker};

  state.symbols = {
      makeSymbol(1U, 1U, "coreFn", "int coreFn()", ultra::ai::SymbolType::Function,
                 10U),
      makeSymbol(2U, 1U, "serviceFn", "int serviceFn()",
                 ultra::ai::SymbolType::Function, 20U),
      makeSymbol(3U, 1U, "appMain", "int appMain()",
                 ultra::ai::SymbolType::Function, 30U),
      makeSymbol(4U, 1U, "workerTask", "int workerTask()",
                 ultra::ai::SymbolType::Function, 40U),
      makeSymbol(2U, 2U, "coreFn", "coreFn()", ultra::ai::SymbolType::Import, 21U),
      makeSymbol(3U, 2U, "serviceFn", "serviceFn()",
                 ultra::ai::SymbolType::Import, 31U),
      makeSymbol(4U, 2U, "serviceFn", "serviceFn()",
                 ultra::ai::SymbolType::Import, 41U),
  };

  state.deps.fileEdges = {
      {3U, 2U},
      {2U, 1U},
      {4U, 2U},
      {3U, 2U},
  };
  state.deps.symbolEdges = {
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(3U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(4U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
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

std::vector<std::string> canonicalStateSignature(
    const ultra::ai::RuntimeState& state) {
  std::vector<std::string> rows;
  rows.reserve(state.files.size() + state.symbols.size() +
               state.deps.fileEdges.size() + state.deps.symbolEdges.size() +
               state.symbolIndex.size());

  for (const ultra::ai::FileRecord& file : state.files) {
    rows.push_back("file|" + std::to_string(file.fileId) + "|" + file.path);
  }
  for (const ultra::ai::SymbolRecord& symbol : state.symbols) {
    rows.push_back("symbol|" + std::to_string(symbol.symbolId) + "|" +
                   std::to_string(symbol.fileId) + "|" + symbol.name + "|" +
                   symbol.signature + "|" +
                   std::to_string(static_cast<int>(symbol.symbolType)) + "|" +
                   std::to_string(static_cast<int>(symbol.visibility)) + "|" +
                   std::to_string(symbol.lineNumber));
  }
  for (const ultra::ai::FileDependencyEdge& edge : state.deps.fileEdges) {
    rows.push_back("file_edge|" + std::to_string(edge.fromFileId) + "->" +
                   std::to_string(edge.toFileId));
  }
  for (const ultra::ai::SymbolDependencyEdge& edge : state.deps.symbolEdges) {
    rows.push_back("symbol_edge|" + std::to_string(edge.fromSymbolId) + "->" +
                   std::to_string(edge.toSymbolId));
  }
  for (const auto& [name, node] : state.symbolIndex) {
    std::vector<std::string> usedIn(node.usedInFiles.begin(), node.usedInFiles.end());
    std::sort(usedIn.begin(), usedIn.end());
    std::string usedConcat;
    for (const std::string& path : usedIn) {
      usedConcat += path + ";";
    }
    rows.push_back("index|" + name + "|" + node.definedIn + "|" + usedConcat);
  }

  std::sort(rows.begin(), rows.end());
  return rows;
}

}  // namespace

TEST(ImpactPredictionEngineTest, PredictSymbolImpactReturnsAffectedFiles) {
  ultra::engine::impact::ImpactPredictionEngine engine(makeImpactState(false));

  const auto prediction = engine.predictSymbolImpact("coreFn");
  EXPECT_EQ(prediction.affectedFiles,
            std::vector<std::string>(
                {"app.cpp", "core.cpp", "service.cpp", "worker.cpp"}));
  EXPECT_EQ(prediction.impactRegion, prediction.affectedFiles);
}

TEST(ImpactPredictionEngineTest, PredictFileImpactReturnsAffectedSymbols) {
  ultra::engine::impact::ImpactPredictionEngine engine(makeImpactState(false));

  const auto prediction = engine.predictFileImpact("core.cpp");
  EXPECT_EQ(prediction.affectedFiles,
            std::vector<std::string>(
                {"app.cpp", "core.cpp", "service.cpp", "worker.cpp"}));
  EXPECT_EQ(prediction.affectedSymbols,
            std::vector<std::string>(
                {"appMain", "coreFn", "serviceFn", "workerTask"}));
}

TEST(ImpactPredictionEngineTest, RiskEvaluatorProducesDeterministicScores) {
  ultra::engine::impact::ImpactPredictionEngine engineA(makeImpactState(false));
  ultra::engine::impact::ImpactPredictionEngine engineB(makeImpactState(true));

  const auto predictionA = engineA.predictSymbolImpact("coreFn");
  const auto predictionB = engineB.predictSymbolImpact("coreFn");

  EXPECT_EQ(predictionA.risk.scoreMicros, predictionB.risk.scoreMicros);
  EXPECT_DOUBLE_EQ(predictionA.risk.score, predictionB.risk.score);
  EXPECT_EQ(predictionA.affectedFiles, predictionB.affectedFiles);
  EXPECT_EQ(predictionA.affectedSymbols, predictionB.affectedSymbols);
}

TEST(ImpactPredictionEngineTest, ChangeSimulationDoesNotMutateRuntimeState) {
  ultra::ai::RuntimeState state = makeImpactState(false);
  const std::vector<std::string> before = canonicalStateSignature(state);
  ultra::engine::impact::ImpactPredictionEngine engine(state);

  const auto simulation = engine.simulateSymbolChange("coreFn");

  EXPECT_FALSE(simulation.runtimeStateMutated);
  EXPECT_EQ(before, canonicalStateSignature(state));
  EXPECT_FALSE(simulation.potentialBreakages.empty());
}

TEST(ImpactPredictionEngineTest, ImpactTraversalRespectsBoundaries) {
  const std::map<std::uint32_t, std::vector<std::uint32_t>> forwardAdjacency = {
      {2U, {1U}},
      {3U, {2U}},
      {4U, {2U}},
  };
  const std::map<std::uint32_t, std::vector<std::uint32_t>> reverseAdjacency = {
      {1U, {2U}},
      {2U, {3U, 4U}},
  };

  ultra::engine::impact::ImpactGraphTraversal::Request<std::uint32_t> request;
  request.startNodes = {1U};
  request.direction = ultra::engine::impact::TraversalDirection::Reverse;
  request.maxDepth = 2U;
  request.maxNodes = 16U;
  request.includeStartNodes = false;
  request.allowedNodes = {2U, 4U};

  const auto result = ultra::engine::impact::ImpactGraphTraversal::traverse(
      request, forwardAdjacency, reverseAdjacency);

  EXPECT_EQ(result.orderedNodes, std::vector<std::uint32_t>({2U, 4U}));
  ASSERT_EQ(result.depthByNode.at(2U), 1U);
  ASSERT_EQ(result.depthByNode.at(4U), 2U);
}
