#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "core/state_manager.h"
#include "runtime/intent/IntentEvaluator.h"

#include <algorithm>
#include <cstdint>
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

ultra::ai::RuntimeState makeIntentState() {
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
      makeSymbol(1U, 1U, "coreFn", "int coreFn()",
                 ultra::ai::SymbolType::Function, 10U),
      makeSymbol(2U, 1U, "serviceFn", "int serviceFn()",
                 ultra::ai::SymbolType::Function, 20U),
      makeSymbol(3U, 1U, "appMain", "int appMain()",
                 ultra::ai::SymbolType::Function, 30U),
      makeSymbol(4U, 1U, "workerTask", "int workerTask()",
                 ultra::ai::SymbolType::Function, 40U),
      makeSymbol(2U, 2U, "coreFn", "coreFn()",
                 ultra::ai::SymbolType::Import, 21U),
      makeSymbol(3U, 2U, "serviceFn", "serviceFn()",
                 ultra::ai::SymbolType::Import, 31U),
      makeSymbol(4U, 2U, "serviceFn", "serviceFn()",
                 ultra::ai::SymbolType::Import, 41U),
  };

  ultra::ai::SymbolNode coreNode;
  coreNode.name = "coreFn";
  coreNode.definedIn = "core.cpp";
  coreNode.usedInFiles = {"service.cpp"};
  coreNode.centrality = 0.6;
  state.symbolIndex["coreFn"] = coreNode;

  ultra::ai::SymbolNode serviceNode;
  serviceNode.name = "serviceFn";
  serviceNode.definedIn = "service.cpp";
  serviceNode.usedInFiles = {"app.cpp", "worker.cpp"};
  serviceNode.centrality = 0.5;
  state.symbolIndex["serviceFn"] = serviceNode;

  ultra::ai::SymbolNode appNode;
  appNode.name = "appMain";
  appNode.definedIn = "app.cpp";
  appNode.usedInFiles = {};
  appNode.centrality = 0.4;
  state.symbolIndex["appMain"] = appNode;

  ultra::ai::SymbolNode workerNode;
  workerNode.name = "workerTask";
  workerNode.definedIn = "worker.cpp";
  workerNode.usedInFiles = {};
  workerNode.centrality = 0.3;
  state.symbolIndex["workerTask"] = workerNode;

  state.deps.fileEdges = {
      {3U, 2U},
      {2U, 1U},
      {4U, 2U},
  };
  state.deps.symbolEdges = {
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(3U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(4U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
  };

  return state;
}

bool planLess(const ultra::runtime::intent::PlanScore& left,
              const ultra::runtime::intent::PlanScore& right) {
  if (left.accepted != right.accepted) {
    return left.accepted && !right.accepted;
  }
  if (left.score != right.score) {
    return left.score > right.score;
  }
  if (left.riskClassification != right.riskClassification) {
    return ultra::runtime::intent::riskRank(left.riskClassification) <
           ultra::runtime::intent::riskRank(right.riskClassification);
  }
  if (left.estimatedImpactRadius != right.estimatedImpactRadius) {
    return left.estimatedImpactRadius < right.estimatedImpactRadius;
  }
  if (left.estimatedTokenUsage != right.estimatedTokenUsage) {
    return left.estimatedTokenUsage < right.estimatedTokenUsage;
  }
  if (left.strategy.impact.dependencyDepth != right.strategy.impact.dependencyDepth) {
    return left.strategy.impact.dependencyDepth <
           right.strategy.impact.dependencyDepth;
  }
  if (left.determinismScore != right.determinismScore) {
    return left.determinismScore > right.determinismScore;
  }
  return left.strategy.name < right.strategy.name;
}

}  // namespace

TEST(IntentRuntime, IntentEvaluationProducesSortedStrategies) {
  ultra::core::StateManager manager;
  manager.replaceState(makeIntentState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(512U);

  ultra::runtime::intent::Intent intent;
  intent.goal.type = ultra::runtime::intent::GoalType::RefactorModule;
  intent.goal.target = "service.cpp";
  intent.constraints.maxImpactDepth = 3U;
  intent.constraints.maxFilesChanged = 6U;
  intent.constraints.tokenBudget = 512U;
  intent.risk = ultra::runtime::intent::RiskTolerance::MEDIUM;
  intent.options.allowCrossModuleMove = true;
  intent.options.allowPublicAPIChange = true;

  ultra::runtime::intent::IntentEvaluator evaluator;
  const ultra::runtime::intent::IntentEvaluation evaluation =
      evaluator.evaluateIntent(intent, state);

  ASSERT_EQ(evaluation.orderedTasks,
            std::vector<std::string>({"impact_prediction",
                                      "context_extraction",
                                      "refactor_module",
                                      "move_across_modules",
                                      "update_public_api",
                                      "update_references",
                                      "validate_plan"}));
  ASSERT_FALSE(evaluation.rankedPlans.empty());
  for (std::size_t index = 1U; index < evaluation.rankedPlans.size(); ++index) {
    EXPECT_EQ(evaluation.rankedPlans[index - 1U].rank, index);
    EXPECT_FALSE(planLess(evaluation.rankedPlans[index],
                          evaluation.rankedPlans[index - 1U]));
  }
}

TEST(IntentRuntime, IntentRiskToleranceIsRespected) {
  ultra::core::StateManager manager;
  manager.replaceState(makeIntentState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(512U);

  ultra::runtime::intent::Intent intent;
  intent.goal.type = ultra::runtime::intent::GoalType::RefactorModule;
  intent.goal.target = "service.cpp";
  intent.constraints.maxImpactDepth = 1U;
  intent.constraints.maxFilesChanged = 1U;
  intent.constraints.tokenBudget = 64U;
  intent.risk = ultra::runtime::intent::RiskTolerance::LOW;
  intent.options.allowCrossModuleMove = true;
  intent.options.allowPublicAPIChange = true;

  ultra::runtime::intent::IntentEvaluator evaluator;
  const ultra::runtime::intent::IntentEvaluation evaluation =
      evaluator.evaluateIntent(intent, state);

  ASSERT_FALSE(evaluation.rankedPlans.empty());
  bool rejectedHighRiskPlan = false;
  for (const ultra::runtime::intent::PlanScore& plan : evaluation.rankedPlans) {
    if (plan.riskClassification == ultra::runtime::intent::RiskTolerance::HIGH) {
      rejectedHighRiskPlan = true;
      EXPECT_FALSE(plan.accepted);
      EXPECT_EQ(plan.executionMode,
                ultra::runtime::intent::ExecutionMode::Reject);
    }
  }
  EXPECT_TRUE(rejectedHighRiskPlan);
}

TEST(IntentRuntime, IntentStrategiesUseImpactPrediction) {
  ultra::core::StateManager manager;
  manager.replaceState(makeIntentState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(512U);

  ultra::runtime::intent::Intent intent;
  intent.goal.type = ultra::runtime::intent::GoalType::ModifySymbol;
  intent.goal.target = "coreFn";
  intent.constraints.maxImpactDepth = 2U;
  intent.constraints.maxFilesChanged = 4U;
  intent.constraints.tokenBudget = 512U;
  intent.risk = ultra::runtime::intent::RiskTolerance::MEDIUM;
  intent.options.allowRename = true;
  intent.options.allowSignatureChange = true;

  ultra::runtime::intent::IntentEvaluator evaluator;
  const ultra::runtime::intent::IntentEvaluation evaluation =
      evaluator.evaluateIntent(intent, state);

  ASSERT_FALSE(evaluation.rankedPlans.empty());
  for (const ultra::runtime::intent::PlanScore& plan : evaluation.rankedPlans) {
    EXPECT_TRUE(plan.usesImpactPrediction);
  }
}
