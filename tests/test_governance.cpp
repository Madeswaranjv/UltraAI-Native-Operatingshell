#include <gtest/gtest.h>

#include "ai/RuntimeState.h"
#include "core/state_manager.h"
#include "runtime/CPUGovernor.h"
#include "runtime/cognitive/ExecutionKernel.h"
#include "runtime/cognitive/contract_enforcement.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

std::string workloadName(const int index) {
  return "governance.layer18." + std::to_string(index);
}

int riskRank(const ultra::runtime::RiskLevel risk) {
  return static_cast<int>(risk);
}

}  // namespace

TEST(GovernanceEngine, ValidToolAllowed) {
  ultra::runtime::CPUGovernor::instance().enterIdle();
  ultra::core::StateManager manager;
  ultra::runtime::ExecutionKernel kernel(manager);

  const ultra::runtime::GovernanceDecision decision =
      kernel.evaluate_action("get_status", {});

  EXPECT_TRUE(decision.allowed);
  EXPECT_GE(decision.confidence, 0.4F);
  EXPECT_NE(decision.risk, ultra::runtime::RiskLevel::Critical);
}

TEST(GovernanceEngine, InvalidSymbolBlocked) {
  ultra::runtime::CPUGovernor::instance().enterIdle();
  ultra::core::StateManager manager;
  ultra::runtime::ExecutionKernel kernel(manager);

  const ultra::runtime::GovernanceDecision decision =
      kernel.evaluate_action("query_symbol",
                             {{"target", "__ultra_nonexistent_symbol__"}});

  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.risk, ultra::runtime::RiskLevel::Critical);
}

TEST(GovernanceEngine, HighRiskBlocked) {
  ultra::runtime::CPUGovernor::instance().enterIdle();
  ultra::core::StateManager manager;
  ultra::runtime::ExecutionKernel kernel(manager);

  const ultra::runtime::GovernanceDecision decision =
      kernel.evaluate_action("get_status", {{"unexpected", "value"}});

  EXPECT_FALSE(decision.allowed);
  EXPECT_EQ(decision.risk, ultra::runtime::RiskLevel::High);
}

TEST(GovernanceEngine, ConfidenceThresholdEnforced) {
  ultra::runtime::CPUGovernor::instance().enterIdle();
  ultra::core::StateManager manager;
  ultra::runtime::ExecutionKernel kernel(manager);

  const ultra::runtime::GovernanceDecision decision =
      kernel.evaluate_action("query_symbol", {});

  EXPECT_FALSE(decision.allowed);
  EXPECT_LT(decision.confidence, 0.4F);
}

TEST(GovernanceEngine, CpuGovernorInfluencesDecision) {
  ultra::runtime::CPUGovernor& governor = ultra::runtime::CPUGovernor::instance();
  ultra::core::StateManager manager;
  ultra::runtime::ExecutionKernel kernel(manager);

  governor.enterIdle();
  const ultra::runtime::GovernanceDecision idleDecision =
      kernel.evaluate_action("get_status", {});

  governor.exitIdle();
  const std::size_t burstCount = std::max<std::size_t>(
      4U, governor.recommendedThreadCount(32U, "governance.tool_execution") + 4U);

  std::vector<std::string> workloads;
  workloads.reserve(burstCount);
  for (std::size_t index = 0U; index < burstCount; ++index) {
    const std::string name = workloadName(static_cast<int>(index));
    governor.registerWorkload(name);
    workloads.push_back(name);
  }

  const ultra::runtime::GovernanceDecision loadedDecision =
      kernel.evaluate_action("get_status", {});

  for (const std::string& name : workloads) {
    governor.recordExecutionTime(name, 180.0);
  }
  governor.enterIdle();

  EXPECT_LE(loadedDecision.confidence, idleDecision.confidence);
  EXPECT_GE(riskRank(loadedDecision.risk), riskRank(idleDecision.risk));
}

TEST(GovernanceEngine, ExecutionKernelGatesBlockedToolExecution) {
  ultra::runtime::CPUGovernor::instance().enterIdle();
  ultra::core::StateManager manager;
  manager.replaceState(ultra::ai::RuntimeState{});
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(256U);
  ultra::runtime::ExecutionKernel kernel(manager);

  ultra::runtime::Action action;
  action.id = "task.blocked_tool";
  action.type = ultra::runtime::ActionType::ToolExecution;
  action.toolName = "query_symbol";
  action.toolArgs = {{"target", "__ultra_nonexistent_symbol__"}};
  action.branch = state.snapshot.branch.toString();
  action.snapshotVersion = state.snapshot.version;

  const ultra::runtime::contracts::ScopedTaskGraphAuthorization authorization(
      action.id);
  const ultra::runtime::Result result = kernel.execute(action, state);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.message.find("Governance blocked"), std::string::npos);
}
