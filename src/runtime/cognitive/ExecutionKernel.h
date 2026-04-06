#pragma once

#include "CognitiveState.h"
#include "tools/ToolExecutor.h"

#include "../../ai/orchestration/OrchestrationContext.h"
#include "../../ai/model/ModelRequest.h"
#include "../../ai/RuntimeState.h"
#include "../../engine/weight_engine.h"
#include "../../memory/StateSnapshot.h"
#include "../../memory/lru_manager.h"
#include "../governance/Policy.h"
#include "../intent/Intent.h"
#include "../intent/Strategy.h"

#include <external/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ultra::core {
class StateManager;
}

namespace ultra::ai::model {
}

namespace ultra::ai::orchestration {
class IMultiModelOrchestrator;
}

namespace ultra::runtime {

enum class ActionType : std::uint8_t {
  Mutation = 0U,
  ImpactPrediction = 1U,
  ContextExtraction = 2U,
  BranchDiff = 3U,
  SimulateChange = 4U,
  IntentEvaluation = 5U,
  ModelGenerate = 6U,
  ToolExecution = 7U
};

enum class RiskLevel : std::uint8_t {
  Low = 0U,
  Medium = 1U,
  High = 2U,
  Critical = 3U
};

struct GovernanceDecision {
  bool allowed{false};
  RiskLevel risk{RiskLevel::Critical};
  float confidence{0.0F};
  std::string reason;
};

struct Action {
  ActionType type{ActionType::Mutation};
  std::string id;
  std::string target;
  std::string branch;
  std::string toolName;
  std::map<std::string, std::string> toolArgs;
  std::uint64_t snapshotVersion{0U};
  double riskScore{0.0};
  double confidenceScore{0.0};
  std::string modelProvider;
  std::optional<intent::Intent> intentRequest;
  std::optional<ai::model::ModelRequest> modelRequest;
  std::optional<ai::orchestration::OrchestrationContext> orchestrationContext;
  std::optional<governance::Policy> policy;
  std::optional<memory::StateSnapshot> comparisonSnapshot;
  std::function<bool(ai::RuntimeState&,
                     engine::WeightEngine&,
                     memory::LruManager&)>
      mutation;
};

struct Result {
  bool ok{false};
  bool applied{false};
  bool rolledBack{false};
  ActionType type{ActionType::Mutation};
  RiskLevel risk{RiskLevel::Low};
  std::uint64_t queueOrder{0U};
  std::uint64_t previousVersion{0U};
  std::uint64_t resultingVersion{0U};
  std::vector<NodeID> impactedNodes;
  std::vector<std::string> normalizedPaths;
  nlohmann::ordered_json payload;
  std::string previousHash;
  std::string resultingHash;
  std::string message;
  std::string text_output;
};

class ExecutionKernel {
 public:
  explicit ExecutionKernel(
      core::StateManager& stateManager,
      std::shared_ptr<ai::orchestration::IMultiModelOrchestrator>
          modelOrchestrator = nullptr);

  [[nodiscard]] static Action buildActionFromStrategy(
      const intent::Action& strategyAction,
      const CognitiveState& state);
  Result execute(const Action& action, const CognitiveState& state);
  Result executeIntent(const intent::Intent& intent,
                       const CognitiveState& state,
                       const governance::Policy& policy = {});

  GovernanceDecision evaluate_action(
      const std::string& tool,
      const std::map<std::string, std::string>& args);

  [[nodiscard]] bool hasToolCognitionLayer() const noexcept;
  [[nodiscard]] bool hasToolRouterLayer() const noexcept;
  [[nodiscard]] bool hasLastResult() const noexcept { return hasLastResult_; }
  [[nodiscard]] const Result& lastResult() const noexcept { return lastResult_; }
  [[nodiscard]] const std::string& lastModelTextOutput() const noexcept {
    return lastModelTextOutput_;
  }
  [[nodiscard]] const std::string& lastSelectedProvider() const noexcept {
    return lastSelectedProvider_;
  }
  [[nodiscard]] const std::string& lastProviderEndpoint() const noexcept {
    return lastProviderEndpoint_;
  }

 private:
  [[nodiscard]] std::string stableActionId(const Action& action) const;
  void validateAction(const Action& action, const CognitiveState& state) const;
  void sortOutputs(Result& result) const;
  Result executeActionLocked(const Action& action, const CognitiveState& state);

  [[nodiscard]] bool validateSymbolWithUltra(const std::string& symbol) const;
  [[nodiscard]] static RiskLevel maxRisk(RiskLevel left, RiskLevel right) noexcept;
  void updateToolFailureHistory(const Action& action, const Result& result);

  core::StateManager& stateManager_;
  std::shared_ptr<ai::orchestration::IMultiModelOrchestrator>
      modelOrchestrator_;
  cognitive::tools::ToolExecutor toolExecutor_{};
  std::map<std::string, std::size_t> toolFailureCounts_;
  std::mutex mutationQueueMutex_;
  std::uint64_t queueCounter_{0U};
  Result lastResult_{};
  bool hasLastResult_{false};
  std::string lastModelTextOutput_{};
  std::string lastSelectedProvider_{};
  std::string lastProviderEndpoint_{};
};

}  // namespace ultra::runtime



