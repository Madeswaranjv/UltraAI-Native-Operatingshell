#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "core/state_manager.h"
#include "runtime/cognitive/ExecutionKernel.h"
#include "runtime/cognitive/micro_planner.h"
#include "runtime/cognitive/strategy_planner.h"
#include "runtime/cognitive/task_graph.h"
#include "runtime/cognitive/contract_enforcement.h"
#include "runtime/cognitive/ultra_loop.h"

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

ultra::ai::RuntimeState makeExecutionState() {
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
  state.files = {core, service, app};

  state.symbols = {
      makeSymbol(1U, 1U, "coreFn", "int coreFn()",
                 ultra::ai::SymbolType::Function, 10U),
      makeSymbol(2U, 1U, "serviceFn", "int serviceFn()",
                 ultra::ai::SymbolType::Function, 20U),
      makeSymbol(3U, 1U, "appMain", "int appMain()",
                 ultra::ai::SymbolType::Function, 30U),
      makeSymbol(2U, 2U, "coreFn", "coreFn()",
                 ultra::ai::SymbolType::Import, 21U),
      makeSymbol(3U, 2U, "serviceFn", "serviceFn()",
                 ultra::ai::SymbolType::Import, 31U),
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
  serviceNode.usedInFiles = {"app.cpp"};
  serviceNode.centrality = 0.5;
  state.symbolIndex["serviceFn"] = serviceNode;

  ultra::ai::SymbolNode appNode;
  appNode.name = "appMain";
  appNode.definedIn = "app.cpp";
  appNode.centrality = 0.4;
  state.symbolIndex["appMain"] = appNode;

  state.deps.fileEdges = {{3U, 2U}, {2U, 1U}};
  state.deps.symbolEdges = {
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(3U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
  };

  return state;
}

std::vector<std::string> readyTaskIds(ultra::runtime::cognitive::TaskGraph graph) {
  std::vector<std::string> ids;
  for (const auto& node : graph.get_ready_tasks()) {
    ids.push_back(node.id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

class SeedIntentStage final : public ultra::runtime::cognitive::IIntentStage {
 public:
  SeedIntentStage(const ultra::runtime::intent::Intent& intentValue,
                  const ultra::runtime::CognitiveState& state)
      : intent_(intentValue), state_(state) {}

  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    frame.cognitiveState = &state_;
    frame.structuredIntent = intent_;
    frame.hasStructuredIntent = true;
    frame.originalStructuredIntent = intent_;
    frame.hasOriginalStructuredIntent = true;
    frame.intentGoal = intent_.goal.target;
    frame.intentTarget = intent_.goal.target;
    frame.intentBranchId = intent_.constraints.branchScope;
    frame.intentTokenBudget = intent_.constraints.tokenBudget;
    frame.intentImpactDepth = intent_.constraints.maxImpactDepth;
    frame.intentMaxFilesChanged = intent_.constraints.maxFilesChanged;
    frame.intentTolerance = intent_.risk;
    frame.intentAllowPublicApiChange = intent_.options.allowPublicAPIChange;
    frame.intentId = ultra::runtime::intent::toString(intent_.goal.type) + ":" +
                     intent_.goal.target;
    frame.originalIntentId = frame.intentId;
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "seeded intent"};
  }

 private:
  ultra::runtime::intent::Intent intent_;
  const ultra::runtime::CognitiveState& state_;
};

class ConflictStrategyStage final : public ultra::runtime::cognitive::IStrategyStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    frame.strategyId = "conflict_strategy";

    ultra::runtime::cognitive::TaskPayload good;
    good.kind = ultra::runtime::cognitive::TaskPayloadKind::Action;
    good.action.type = ultra::runtime::ActionType::ImpactPrediction;
    good.action.target = "coreFn";
    good.action.id = "winner";
    good.action.riskScore = 0.10;
    good.plannedAction = ultra::runtime::intent::Action{
        ultra::runtime::intent::ActionKind::ReduceImpactRadius,
        "coreFn",
        "Minimal context extraction.",
        1U,
        1U,
        false,
    };

    ultra::runtime::cognitive::TaskPayload bad;
    bad.kind = ultra::runtime::cognitive::TaskPayloadKind::Action;
    bad.action.type = ultra::runtime::ActionType::SimulateChange;
    bad.action.target = "coreFn";
    bad.action.id = "loser";
    bad.action.riskScore = 0.90;
    bad.plannedAction = ultra::runtime::intent::Action{
        ultra::runtime::intent::ActionKind::ChangeSignature,
        "coreFn",
        "High-risk signature change.",
        4U,
        4U,
        true,
    };

    frame.microTaskPayloads = {good, bad};
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "seeded conflicting actions"};
  }
};

class PassThroughMicroPlanningStage final
    : public ultra::runtime::cognitive::IMicroPlanningStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "micro planning ok"};
  }
};

class RecordingMicroPlanningStage final
    : public ultra::runtime::cognitive::IMicroPlanningStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    strategyIds.push_back(frame.strategyId);
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "recorded micro planning"};
  }

  std::vector<std::string> strategyIds;
};

class ApproveGovernanceStage final
    : public ultra::runtime::cognitive::IGovernanceStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "governance ok"};
  }
};

class RecoveryPassStage final : public ultra::runtime::cognitive::IRecoveryStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "recovery unused"};
  }
};

class RetryVerificationStage final
    : public ultra::runtime::cognitive::IVerificationStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    if (calls_++ == 0U) {
      return {true, ultra::runtime::cognitive::StageSignal::Retry,
              "verification requested targeted retry"};
    }
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "verification passed"};
  }

 private:
  std::size_t calls_{0U};
};

class ReplanThenPassVerificationStage final
    : public ultra::runtime::cognitive::IVerificationStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    if (calls_++ == 0U) {
      return {false, ultra::runtime::cognitive::StageSignal::Replan,
              "verification forced feedback-driven replanning"};
    }
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "verification passed"};
  }

 private:
  std::size_t calls_{0U};
};

class ReanchorOnceReflectionStage final
    : public ultra::runtime::cognitive::IReflectionStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    ultra::runtime::cognitive::IntentConsistencyRecord record;
    record.iteration = frame.iteration;
    record.planId = frame.planId;
    record.consistent = calls_ != 0U;
    record.reason = record.consistent ? "intent remains aligned"
                                      : "test induced intent drift";
    frame.intentConsistencyLog.push_back(record);
    frame.intentConsistent = record.consistent;
    frame.intentConsistencyReason = record.reason;
    frame.reanchorRequested = !record.consistent;
    ++calls_;
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            record.reason};
  }

 private:
  std::size_t calls_{0U};
};

class StableReflectionStage final
    : public ultra::runtime::cognitive::IReflectionStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    ultra::runtime::cognitive::IntentConsistencyRecord record;
    record.iteration = frame.iteration;
    record.planId = frame.planId;
    record.consistent = true;
    record.reason = "intent remains aligned";
    frame.intentConsistencyLog.push_back(record);
    frame.intentConsistent = true;
    frame.intentConsistencyReason = record.reason;
    frame.reanchorRequested = false;
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            record.reason};
  }
};

class PassReplanningStage final
    : public ultra::runtime::cognitive::IReplanningStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "replanning ok"};
  }
};

class PassMemoryStage final : public ultra::runtime::cognitive::IMemoryStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "memory ok"};
  }
};

class ContractViolatingGovernanceStage final
    : public ultra::runtime::cognitive::IGovernanceStage {
 public:
  explicit ContractViolatingGovernanceStage(ultra::core::StateManager& manager)
      : manager_(manager) {}

  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame& frame) override {
    const ultra::runtime::GraphSnapshot snapshot =
        frame.cognitiveState != nullptr ? frame.cognitiveState->snapshot
                                        : manager_.getSnapshot();
    manager_.cognitiveMemory().recordIntentStart(
        "illegal_execute_phase_write", snapshot, 0.5, 0.5,
        "forbidden_execute_write");
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "should not reach"};
  }

 private:
  ultra::core::StateManager& manager_;
};

class RecoveryReplanStage final : public ultra::runtime::cognitive::IRecoveryStage {
 public:
  ultra::runtime::cognitive::StageResult run(
      ultra::runtime::cognitive::UltraLoopFrame&) override {
    ++calls;
    return {true, ultra::runtime::cognitive::StageSignal::Continue,
            "contract violation routed to replanning"};
  }

  std::size_t calls{0U};
};

bool containsTransition(const ultra::runtime::cognitive::UltraLoopReport& report,
                        const ultra::runtime::cognitive::UltraLoopState from,
                        const ultra::runtime::cognitive::UltraLoopState to) {
  return std::any_of(report.transitions.begin(), report.transitions.end(),
                     [from, to](const auto& transition) {
                       return transition.from == from && transition.to == to;
                     });
}

}  // namespace

TEST(TaskGraphRepair, ReopenTaskPreservesCompletedDependencies) {
  ultra::runtime::cognitive::TaskGraph graph;

  ultra::runtime::cognitive::TaskNode a;
  a.id = "a";
  EXPECT_TRUE(graph.add_task(std::move(a)));

  ultra::runtime::cognitive::TaskNode b;
  b.id = "b";
  b.dependencies = {"a"};
  EXPECT_TRUE(graph.add_task(std::move(b)));

  ultra::runtime::cognitive::TaskNode c;
  c.id = "c";
  c.dependencies = {"b"};
  EXPECT_TRUE(graph.add_task(std::move(c)));

  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"a"}));
  EXPECT_TRUE(graph.mark_running("a"));
  EXPECT_TRUE(graph.mark_completed("a"));
  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"b"}));
  EXPECT_TRUE(graph.mark_running("b"));
  EXPECT_TRUE(graph.mark_completed("b"));
  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"c"}));
  EXPECT_TRUE(graph.mark_running("c"));
  EXPECT_TRUE(graph.mark_completed("c"));

  EXPECT_TRUE(graph.reopen_task("b"));
  EXPECT_EQ(readyTaskIds(graph), std::vector<std::string>({"b"}));
  EXPECT_TRUE(graph.has_pending_tasks());

  EXPECT_TRUE(graph.mark_running("b"));
  EXPECT_TRUE(graph.mark_completed("b"));
  EXPECT_EQ(readyTaskIds(graph), std::vector<std::string>({"c"}));
}

TEST(TaskGraphRepair, OverlayKeepsBaseUntouchedUntilVerifiedMerge) {
  ultra::runtime::cognitive::TaskGraph graph;

  ultra::runtime::cognitive::TaskNode a;
  a.id = "a";
  ASSERT_TRUE(graph.add_task(std::move(a)));

  ultra::runtime::cognitive::TaskNode b;
  b.id = "b";
  b.dependencies = {"a"};
  ASSERT_TRUE(graph.add_task(std::move(b)));

  ultra::runtime::cognitive::TaskNode c;
  c.id = "c";
  c.dependencies = {"b"};
  ASSERT_TRUE(graph.add_task(std::move(c)));

  ultra::runtime::cognitive::TaskNode x;
  x.id = "x";
  ASSERT_TRUE(graph.add_task(std::move(x)));

  ultra::runtime::cognitive::TaskNode y;
  y.id = "y";
  y.dependencies = {"x"};
  ASSERT_TRUE(graph.add_task(std::move(y)));

  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"a", "x"}));
  ASSERT_TRUE(graph.mark_running("a"));
  ASSERT_TRUE(graph.mark_completed("a"));
  ASSERT_TRUE(graph.mark_running("x"));
  ASSERT_TRUE(graph.mark_completed("x"));
  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"b", "y"}));
  ASSERT_TRUE(graph.mark_running("b"));
  ASSERT_TRUE(graph.mark_failed("b"));
  ASSERT_TRUE(graph.mark_running("y"));
  ASSERT_TRUE(graph.mark_completed("y"));

  auto overlay = graph.create_repair_overlay({"b"}, 1U);
  ASSERT_FALSE(overlay.empty());
  EXPECT_EQ(overlay.metadata().affectedNodes,
            std::vector<std::string>({"b", "c"}));

  ASSERT_TRUE(overlay.reopen_task("b"));
  ASSERT_TRUE(overlay.verify());
  EXPECT_EQ(graph.failed_tasks(), std::vector<std::string>({"b"}));
  EXPECT_TRUE(readyTaskIds(graph).empty());

  const auto merged = graph.merge_repair_overlay(overlay);
  ASSERT_TRUE(merged.has_value());
  EXPECT_TRUE(merged->failed_tasks().empty());
  EXPECT_EQ(readyTaskIds(*merged), std::vector<std::string>({"b"}));
  EXPECT_EQ(graph.failed_tasks(), std::vector<std::string>({"b"}));
}

TEST(TaskGraphRepair, BaseMutationDuringRepairPhaseThrowsOverlayBypass) {
  ultra::runtime::cognitive::TaskGraph graph;

  ultra::runtime::cognitive::TaskNode a;
  a.id = "a";
  ASSERT_TRUE(graph.add_task(std::move(a)));

  ultra::runtime::cognitive::TaskNode b;
  b.id = "b";
  b.dependencies = {"a"};
  ASSERT_TRUE(graph.add_task(std::move(b)));

  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"a"}));
  ASSERT_TRUE(graph.mark_running("a"));
  ASSERT_TRUE(graph.mark_completed("a"));
  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"b"}));
  ASSERT_TRUE(graph.mark_running("b"));
  ASSERT_TRUE(graph.mark_failed("b"));

  const ultra::runtime::contracts::ScopedLoopPhase phase(
      ultra::runtime::contracts::LoopPhase::PARTIAL_REPAIR);
  EXPECT_THROW(graph.reopen_task("b"),
               ultra::runtime::contracts::ContractViolationException);
  EXPECT_THROW(graph.reset_failed("b"),
               ultra::runtime::contracts::ContractViolationException);
}

TEST(TaskGraphRepair, RejectsMergingRepairOverlayAgainstStaleBaseSnapshot) {
  ultra::runtime::cognitive::TaskGraph graph;

  ultra::runtime::cognitive::TaskNode a;
  a.id = "a";
  ASSERT_TRUE(graph.add_task(std::move(a)));

  ultra::runtime::cognitive::TaskNode b;
  b.id = "b";
  b.dependencies = {"a"};
  ASSERT_TRUE(graph.add_task(std::move(b)));

  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"a"}));
  ASSERT_TRUE(graph.mark_running("a"));
  ASSERT_TRUE(graph.mark_completed("a"));
  ASSERT_EQ(readyTaskIds(graph), std::vector<std::string>({"b"}));
  ASSERT_TRUE(graph.mark_running("b"));
  ASSERT_TRUE(graph.mark_failed("b"));

  auto overlay = graph.create_repair_overlay({"b"}, 1U);
  ASSERT_TRUE(overlay.reopen_task("b"));
  ASSERT_TRUE(overlay.verify());
  ASSERT_TRUE(graph.reset_failed("b"));

  EXPECT_THROW((void)graph.merge_repair_overlay(overlay),
               ultra::runtime::contracts::ContractViolationException);
}

TEST(UltraLoopHelpers, TaskGraphStructuralHashIsDeterministic) {
  ultra::runtime::cognitive::TaskGraph left;
  ultra::runtime::cognitive::TaskNode a;
  a.id = "a";
  ASSERT_TRUE(left.add_task(std::move(a)));

  ultra::runtime::cognitive::TaskNode b;
  b.id = "b";
  b.dependencies = {"a"};
  ASSERT_TRUE(left.add_task(std::move(b)));

  ultra::runtime::cognitive::TaskGraph right;
  ultra::runtime::cognitive::TaskNode a2;
  a2.id = "a";
  ASSERT_TRUE(right.add_task(std::move(a2)));

  ultra::runtime::cognitive::TaskNode b2;
  b2.id = "b";
  ASSERT_TRUE(right.add_task(std::move(b2)));
  ASSERT_TRUE(right.add_dependency("a", "b"));

  EXPECT_EQ(left.structural_hash(), right.structural_hash());

  ultra::runtime::cognitive::TaskGraph different;
  ultra::runtime::cognitive::TaskNode x;
  x.id = "x";
  ASSERT_TRUE(different.add_task(std::move(x)));
  ultra::runtime::cognitive::TaskNode y;
  y.id = "y";
  y.dependencies = {"x"};
  ASSERT_TRUE(different.add_task(std::move(y)));

  EXPECT_NE(left.structural_hash(), different.structural_hash());
}

TEST(UltraLoopHelpers, ArbitrationSelectsBestConflictCandidate) {
  ultra::runtime::cognitive::UltraLoopFrame frame;
  frame.iteration = 1U;
  frame.hasStructuredIntent = true;
  frame.structuredIntent.goal.type = ultra::runtime::intent::GoalType::ReduceImpactRadius;
  frame.structuredIntent.goal.target = "coreFn";
  frame.structuredIntent.constraints.maxFilesChanged = 2U;
  frame.structuredIntent.constraints.maxImpactDepth = 2U;
  frame.structuredIntent.constraints.tokenBudget = 256U;
  frame.hasOriginalStructuredIntent = true;
  frame.originalStructuredIntent = frame.structuredIntent;

  ConflictStrategyStage strategyStage;
  ASSERT_TRUE(strategyStage.run(frame).success);

  ultra::runtime::cognitive::DeterministicArbitrationStage arbitrationStage;
  const auto result = arbitrationStage.run(frame);
  ASSERT_TRUE(result.success);
  ASSERT_EQ(frame.microTaskPayloads.size(), 1U);
  EXPECT_EQ(frame.microTaskPayloads.front().action.id, "winner");
  ASSERT_EQ(frame.arbitrationLog.size(), 1U);
  EXPECT_EQ(frame.arbitrationLog.front().conflictCount, 1U);
}

TEST(UltraLoopHelpers, IntentConsistencyDetectsDrift) {
  ultra::runtime::cognitive::UltraLoopFrame frame;
  frame.iteration = 1U;
  frame.planId = "plan_1";
  frame.hasStructuredIntent = true;
  frame.structuredIntent.goal.type = ultra::runtime::intent::GoalType::ModifySymbol;
  frame.structuredIntent.goal.target = "coreFn";
  frame.structuredIntent.constraints.maxFilesChanged = 2U;
  frame.structuredIntent.constraints.maxImpactDepth = 2U;
  frame.structuredIntent.constraints.tokenBudget = 256U;
  frame.hasOriginalStructuredIntent = true;
  frame.originalStructuredIntent = frame.structuredIntent;

  ultra::runtime::cognitive::TaskPayload payload;
  payload.kind = ultra::runtime::cognitive::TaskPayloadKind::Action;
  payload.action.id = "drift";
  payload.action.target = "otherFn";
  payload.plannedAction = ultra::runtime::intent::Action{
      ultra::runtime::intent::ActionKind::ChangeSignature,
      "otherFn",
      "drifted change",
      6U,
      5U,
      true,
  };
  frame.microTaskPayloads = {payload};

  const auto consistency = ultra::runtime::cognitive::evaluateIntentConsistency(frame);
  EXPECT_FALSE(consistency.consistent);
  EXPECT_NE(consistency.reason.find("diverges"), std::string::npos);
}

TEST(UltraLoop, TransitionsThroughNewArchitecturalStates) {
  ultra::core::StateManager manager;
  manager.replaceState(makeExecutionState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(512U);

  ultra::runtime::intent::Intent intentValue;
  intentValue.goal.type = ultra::runtime::intent::GoalType::ReduceImpactRadius;
  intentValue.goal.target = "coreFn";
  intentValue.constraints.maxImpactDepth = 2U;
  intentValue.constraints.maxFilesChanged = 2U;
  intentValue.constraints.tokenBudget = 512U;
  intentValue.constraints.determinismRequired = true;
  intentValue.risk = ultra::runtime::intent::RiskTolerance::HIGH;

  ultra::runtime::ExecutionKernel kernel(manager);
  ultra::runtime::cognitive::MicroPlanner microPlanner;
  SeedIntentStage intentStage(intentValue, state);
  ConflictStrategyStage strategyStage;
  ultra::runtime::cognitive::DeterministicArbitrationStage arbitrationStage;
  PassThroughMicroPlanningStage microPlanningStage;
  ApproveGovernanceStage governanceStage;
  RecoveryPassStage recoveryStage;
  RetryVerificationStage verificationStage;
  ReanchorOnceReflectionStage reflectionStage;
  ultra::runtime::cognitive::DeterministicReanchorStage reanchorStage;
  PassReplanningStage replanningStage;
  PassMemoryStage memoryStage;

  ultra::runtime::cognitive::UltraLoopBindings bindings;
  bindings.intent = &intentStage;
  bindings.strategy = &strategyStage;
  bindings.arbitration = &arbitrationStage;
  bindings.microPlanning = &microPlanningStage;
  bindings.microPlanner = &microPlanner;
  bindings.executionKernel = &kernel;
  bindings.governance = &governanceStage;
  bindings.recovery = &recoveryStage;
  bindings.verification = &verificationStage;
  bindings.reflection = &reflectionStage;
  bindings.reanchor = &reanchorStage;
  bindings.replanning = &replanningStage;
  bindings.memory = &memoryStage;

  ultra::runtime::cognitive::UltraLoopConfig config;
  config.maxIterations = 4U;
  config.maxRetriesPerIteration = 2U;
  config.maxStagnationIterations = 2U;
  config.minImprovementDelta = 0.05;
  config.maxPlanRepetitions = 2U;

  ultra::runtime::cognitive::UltraLoop loop(config);
  const auto report = loop.run(bindings);

  ASSERT_TRUE(report.success);
  EXPECT_EQ(report.terminalSignal,
            ultra::runtime::cognitive::StageSignal::SIG_CONVERGENCE_REACHED);
  EXPECT_TRUE(containsTransition(report,
                                 ultra::runtime::cognitive::UltraLoopState::PLAN,
                                 ultra::runtime::cognitive::UltraLoopState::ARBITRATION));
  EXPECT_TRUE(containsTransition(report,
                                 ultra::runtime::cognitive::UltraLoopState::VERIFY,
                                 ultra::runtime::cognitive::UltraLoopState::PARTIAL_REPAIR));
  EXPECT_TRUE(containsTransition(report,
                                 ultra::runtime::cognitive::UltraLoopState::PARTIAL_REPAIR,
                                 ultra::runtime::cognitive::UltraLoopState::EXECUTE));
  EXPECT_TRUE(containsTransition(report,
                                 ultra::runtime::cognitive::UltraLoopState::REFLECT,
                                 ultra::runtime::cognitive::UltraLoopState::RE_ANCHOR));
  EXPECT_TRUE(containsTransition(report,
                                 ultra::runtime::cognitive::UltraLoopState::RE_ANCHOR,
                                 ultra::runtime::cognitive::UltraLoopState::PLAN));
  EXPECT_TRUE(containsTransition(report,
                                 ultra::runtime::cognitive::UltraLoopState::REFLECT,
                                 ultra::runtime::cognitive::UltraLoopState::PLAN));

  ASSERT_FALSE(report.repairs.empty());
  EXPECT_TRUE(std::any_of(report.repairs.begin(), report.repairs.end(),
                          [](const auto& repair) {
                            return repair.site == "VERIFY" &&
                                   repair.verified &&
                                   !repair.overlayId.empty() &&
                                   !repair.affectedNodes.empty();
                          }));
  ASSERT_FALSE(report.arbitration.empty());
  EXPECT_EQ(report.arbitration.front().conflictCount, 1U);
  ASSERT_FALSE(report.intentConsistency.empty());
  EXPECT_FALSE(report.intentConsistency.front().consistent);
}





TEST(UltraLoop, ContractViolationTriggersRecoveryPath) {
  ultra::core::StateManager manager;
  manager.replaceState(makeExecutionState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(512U);

  ultra::runtime::intent::Intent intentValue;
  intentValue.goal.type = ultra::runtime::intent::GoalType::ReduceImpactRadius;
  intentValue.goal.target = "coreFn";
  intentValue.constraints.maxImpactDepth = 2U;
  intentValue.constraints.maxFilesChanged = 2U;
  intentValue.constraints.tokenBudget = 512U;
  intentValue.constraints.determinismRequired = true;
  intentValue.risk = ultra::runtime::intent::RiskTolerance::HIGH;

  ultra::runtime::ExecutionKernel kernel(manager);
  ultra::runtime::cognitive::MicroPlanner microPlanner;
  SeedIntentStage intentStage(intentValue, state);
  ConflictStrategyStage strategyStage;
  ultra::runtime::cognitive::DeterministicArbitrationStage arbitrationStage;
  PassThroughMicroPlanningStage microPlanningStage;
  ContractViolatingGovernanceStage governanceStage(manager);
  RecoveryReplanStage recoveryStage;
  RetryVerificationStage verificationStage;
  ReanchorOnceReflectionStage reflectionStage;
  ultra::runtime::cognitive::DeterministicReanchorStage reanchorStage;
  PassReplanningStage replanningStage;
  PassMemoryStage memoryStage;

  ultra::runtime::cognitive::UltraLoopBindings bindings;
  bindings.intent = &intentStage;
  bindings.strategy = &strategyStage;
  bindings.arbitration = &arbitrationStage;
  bindings.microPlanning = &microPlanningStage;
  bindings.microPlanner = &microPlanner;
  bindings.executionKernel = &kernel;
  bindings.governance = &governanceStage;
  bindings.recovery = &recoveryStage;
  bindings.verification = &verificationStage;
  bindings.reflection = &reflectionStage;
  bindings.reanchor = &reanchorStage;
  bindings.replanning = &replanningStage;
  bindings.memory = &memoryStage;
  bindings.cognitiveMemory = &manager.cognitiveMemory();

  ultra::runtime::cognitive::UltraLoopConfig config;
  config.maxIterations = 1U;
  config.maxRetriesPerIteration = 1U;
  config.maxStagnationIterations = 2U;
  config.minImprovementDelta = 0.05;
  config.maxPlanRepetitions = 2U;

  ultra::runtime::cognitive::UltraLoop loop(config);
  const auto report = loop.run(bindings);

  EXPECT_FALSE(report.success);
  EXPECT_EQ(recoveryStage.calls, 1U);
  EXPECT_TRUE(containsTransition(report,
                                 ultra::runtime::cognitive::UltraLoopState::EXECUTE,
                                 ultra::runtime::cognitive::UltraLoopState::REPLAN));
  EXPECT_TRUE(report.terminatedByIterationCap);
}
TEST(StrategyPlannerFeedback, LowScoreRepeatedPlanForcesVariation) {
  ultra::runtime::cognitive::UltraLoopFrame frame;
  frame.iteration = 2U;
  frame.hasStructuredIntent = true;
  frame.structuredIntent.goal.type = ultra::runtime::intent::GoalType::ModifySymbol;
  frame.structuredIntent.goal.target = "coreFn";
  frame.structuredIntent.constraints.maxFilesChanged = 4U;
  frame.structuredIntent.constraints.maxImpactDepth = 3U;
  frame.structuredIntent.constraints.tokenBudget = 256U;
  frame.structuredIntent.constraints.determinismRequired = true;
  frame.structuredIntent.memory.strategyFeedback.strategyType =
      "deterministic_modify_symbol";
  frame.structuredIntent.memory.strategyFeedback.forceVariation = true;
  frame.structuredIntent.memory.strategyFeedback.avoidRepeatedPlan = true;
  frame.structuredIntent.memory.strategyFeedback.simplifyPlan = true;
  frame.structuredIntent.memory.strategyFeedback.avoidedStrategyType =
      "deterministic_modify_symbol";
  frame.structuredIntent.memory.strategyFeedback.latestScore.failureCount = 2U;
  frame.structuredIntent.memory.strategyFeedback.latestScore.recoveryCost = 0.7;

  ultra::runtime::cognitive::StrategyPlanningStage stage;
  const auto result = stage.run(frame);

  ASSERT_TRUE(result.success);
  EXPECT_NE(frame.strategyId.find("feedback_varied"), std::string::npos);
  ASSERT_FALSE(frame.microTaskPayloads.empty());
  ASSERT_TRUE(frame.microTaskPayloads.front().plannedAction.has_value());
  EXPECT_EQ(frame.microTaskPayloads.front().plannedAction->estimatedFilesChanged,
            1U);
  EXPECT_EQ(
      frame.microTaskPayloads.front().plannedAction->estimatedDependencyDepth,
      1U);
}

TEST(MicroPlannerFeedback, RepeatedFailureIncreasesGranularity) {
  ultra::runtime::cognitive::MicroPlanner planner;
  ultra::runtime::cognitive::TaskPayload payload;
  payload.kind = ultra::runtime::cognitive::TaskPayloadKind::Action;
  payload.action.type = ultra::runtime::ActionType::SimulateChange;
  payload.action.target = "coreFn";

  ultra::runtime::cognitive::MicroPlanInput input;
  input.intentId = "intent";
  input.strategyId = "feedback_varied_modify_symbol";
  input.planId = "plan_2";
  input.taskPayloads = {payload};
  input.memory.strategyFeedback.increaseTaskGranularity = true;
  input.memory.strategyFeedback.forceVariation = true;

  ultra::runtime::cognitive::TaskGraph graph = planner.generate_plan(input);
  ASSERT_EQ(graph.size(), 3U);

  auto ready = graph.get_ready_tasks();
  ASSERT_EQ(ready.size(), 1U);
  EXPECT_EQ(ready.front().payload.action.type,
            ultra::runtime::ActionType::ContextExtraction);
  ASSERT_TRUE(graph.mark_completed(ready.front().id));

  ready = graph.get_ready_tasks();
  ASSERT_EQ(ready.size(), 1U);
  EXPECT_EQ(ready.front().payload.action.type,
            ultra::runtime::ActionType::ImpactPrediction);
}

TEST(UltraLoopFeedback, ReplanningFeedsBackIntoNextStrategy) {
  ultra::core::StateManager manager;
  manager.replaceState(makeExecutionState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(512U);

  ultra::runtime::intent::Intent intentValue;
  intentValue.goal.type = ultra::runtime::intent::GoalType::ReduceImpactRadius;
  intentValue.goal.target = "coreFn";
  intentValue.constraints.maxImpactDepth = 2U;
  intentValue.constraints.maxFilesChanged = 2U;
  intentValue.constraints.tokenBudget = 512U;
  intentValue.constraints.determinismRequired = true;
  intentValue.risk = ultra::runtime::intent::RiskTolerance::MEDIUM;

  ultra::runtime::ExecutionKernel kernel(manager);
  ultra::runtime::cognitive::MicroPlanner microPlanner;
  ultra::runtime::cognitive::StrategyPlanningStage strategyStage;
  SeedIntentStage intentStage(intentValue, state);
  ultra::runtime::cognitive::DeterministicArbitrationStage arbitrationStage;
  RecordingMicroPlanningStage microPlanningStage;
  ApproveGovernanceStage governanceStage;
  RecoveryPassStage recoveryStage;
  ReplanThenPassVerificationStage verificationStage;
  StableReflectionStage reflectionStage;
  ultra::runtime::cognitive::DeterministicReanchorStage reanchorStage;
  PassReplanningStage replanningStage;
  PassMemoryStage memoryStage;

  ultra::runtime::cognitive::UltraLoopBindings bindings;
  bindings.intent = &intentStage;
  bindings.strategy = &strategyStage;
  bindings.arbitration = &arbitrationStage;
  bindings.microPlanning = &microPlanningStage;
  bindings.microPlanner = &microPlanner;
  bindings.executionKernel = &kernel;
  bindings.governance = &governanceStage;
  bindings.recovery = &recoveryStage;
  bindings.verification = &verificationStage;
  bindings.reflection = &reflectionStage;
  bindings.reanchor = &reanchorStage;
  bindings.replanning = &replanningStage;
  bindings.memory = &memoryStage;
  bindings.cognitiveMemory = &manager.cognitiveMemory();

  ultra::runtime::cognitive::UltraLoopConfig config;
  config.maxIterations = 2U;
  config.maxRetriesPerIteration = 1U;
  config.maxStagnationIterations = 2U;
  config.minImprovementDelta = 0.05;
  config.maxPlanRepetitions = 2U;

  ultra::runtime::cognitive::UltraLoop loop(config);
  const auto report = loop.run(bindings);

  EXPECT_TRUE(report.terminatedByIterationCap || report.success);
  ASSERT_GE(microPlanningStage.strategyIds.size(), 2U);
  EXPECT_NE(microPlanningStage.strategyIds.front(),
            microPlanningStage.strategyIds[1]);
  EXPECT_NE(microPlanningStage.strategyIds[1].find("feedback"),
            std::string::npos);
  ASSERT_FALSE(report.strategyFeedback.empty());
  ASSERT_FALSE(report.planPerformance.empty());
  EXPECT_LE(report.planPerformance.size(), 4U);
  EXPECT_EQ(report.strategyFeedback.back().recentPlans.size(),
            report.planPerformance.size());
}

