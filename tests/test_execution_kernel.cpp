#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "core/state_manager.h"
#include "runtime/cognitive/ExecutionKernel.h"
#include "runtime/cognitive/contract_enforcement.h"

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

ultra::ai::RuntimeState makeExecutionState(const bool shuffled) {
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

  if (shuffled) {
    std::reverse(state.files.begin(), state.files.end());
    std::reverse(state.symbols.begin(), state.symbols.end());
    std::reverse(state.deps.fileEdges.begin(), state.deps.fileEdges.end());
    std::reverse(state.deps.symbolEdges.begin(), state.deps.symbolEdges.end());
  }

  return state;
}

}  // namespace

TEST(ExecutionKernel, ExecutionKernelRejectsInvalidSnapshot) {
  ultra::core::StateManager manager;
  manager.replaceState(makeExecutionState(false));
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(256U);

  ultra::runtime::ExecutionKernel kernel(manager);
  ultra::runtime::Action action;
  action.id = "task.invalid_snapshot";
  action.type = ultra::runtime::ActionType::ContextExtraction;
  action.target = "coreFn";
  action.snapshotVersion = state.snapshot.version + 1U;

  const ultra::runtime::contracts::ScopedTaskGraphAuthorization authorization(
      action.id);
  const ultra::runtime::Result result = kernel.execute(action, state);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.message.find("snapshot version"), std::string::npos);
}

TEST(ExecutionKernel, ExecutionKernelProducesDeterministicResults) {
  ultra::core::StateManager managerA;
  ultra::core::StateManager managerB;
  managerA.replaceState(makeExecutionState(false));
  managerB.replaceState(makeExecutionState(true));

  const ultra::runtime::CognitiveState stateA = managerA.createCognitiveState(512U);
  const ultra::runtime::CognitiveState stateB = managerB.createCognitiveState(512U);

  ultra::runtime::ExecutionKernel kernelA(managerA);
  ultra::runtime::ExecutionKernel kernelB(managerB);

  ultra::runtime::Action actionA;
  actionA.id = "task.simulate";
  actionA.type = ultra::runtime::ActionType::SimulateChange;
  actionA.target = "coreFn";
  actionA.snapshotVersion = stateA.snapshot.version;

  ultra::runtime::Action actionB = actionA;
  actionB.snapshotVersion = stateB.snapshot.version;

  ultra::runtime::Result resultA;
  {
    const ultra::runtime::contracts::ScopedTaskGraphAuthorization authorization(
        actionA.id);
    resultA = kernelA.execute(actionA, stateA);
  }
  ultra::runtime::Result resultB;
  {
    const ultra::runtime::contracts::ScopedTaskGraphAuthorization authorization(
        actionB.id);
    resultB = kernelB.execute(actionB, stateB);
  }

  ASSERT_FALSE(resultA.ok);
  ASSERT_FALSE(resultB.ok);
  EXPECT_EQ(resultA.payload.dump(), resultB.payload.dump());
  EXPECT_EQ(resultA.impactedNodes, resultB.impactedNodes);
  EXPECT_EQ(resultA.normalizedPaths, resultB.normalizedPaths);
  EXPECT_EQ(resultA.risk, resultB.risk);
  EXPECT_EQ(resultA.message, resultB.message);
  EXPECT_NE(resultA.message.find("SimulateChange is disabled"), std::string::npos);
}


TEST(ExecutionKernel, RejectsExecutionOutsideTaskGraphAuthorization) {
  ultra::core::StateManager manager;
  manager.replaceState(makeExecutionState(false));
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(256U);

  ultra::runtime::ExecutionKernel kernel(manager);
  ultra::runtime::Action action;
  action.id = "task.unauthorized";
  action.type = ultra::runtime::ActionType::ContextExtraction;
  action.target = "coreFn";
  action.snapshotVersion = state.snapshot.version;

  const ultra::runtime::Result result = kernel.execute(action, state);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.message.find("task-graph authorization"), std::string::npos);
}

TEST(ExecutionKernel, ExecuteIntentEvaluatesButDoesNotExecuteChildActions) {
  ultra::core::StateManager manager;
  manager.replaceState(makeExecutionState(false));
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(512U);

  ultra::runtime::ExecutionKernel kernel(manager);
  ultra::runtime::intent::Intent intentValue;
  intentValue.goal.type = ultra::runtime::intent::GoalType::ModifySymbol;
  intentValue.goal.target = "coreFn";
  intentValue.constraints.tokenBudget = 512U;
  intentValue.constraints.maxImpactDepth = 2U;
  intentValue.constraints.maxFilesChanged = 2U;

  const ultra::runtime::contracts::ScopedTaskGraphAuthorization authorization(
      "task.intent.evaluate");
  const ultra::runtime::Result result = kernel.executeIntent(intentValue, state);

  EXPECT_EQ(result.type, ultra::runtime::ActionType::IntentEvaluation);
  ASSERT_TRUE(result.payload.contains("ordered_tasks"));
  ASSERT_TRUE(result.payload.contains("ranked_plans"));
  ASSERT_TRUE(result.payload.contains("child_results"));
  EXPECT_TRUE(result.payload.at("child_results").is_array());
  EXPECT_TRUE(result.payload.at("child_results").empty());
  EXPECT_FALSE(result.payload.value("tool_router_executed", false));
  EXPECT_TRUE(result.impactedNodes.empty());
  EXPECT_TRUE(result.normalizedPaths.empty());
  EXPECT_TRUE(
      result.payload.value("execution_deferred_to_task_graph", false) || !result.ok);
}
