#pragma once

#include "CognitiveState.h"

#include "../../ai/orchestration/OrchestrationContext.h"
#include "../../ai/model/ModelRequest.h"
#include "../../ai/RuntimeState.h"
#include "../../engine/weight_engine.h"
#include "../../memory/StateSnapshot.h"
#include "../../memory/lru_manager.h"
#include "../governance/Policy.h"
#include "../intent/Intent.h"

#include <external/json.hpp>

#include <cstdint>
#include <functional>
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
  ModelGenerate = 6U
};

enum class RiskLevel : std::uint8_t {
  Low = 0U,
  Medium = 1U,
  High = 2U
};

struct Action {
  ActionType type{ActionType::Mutation};
  std::string id;
  std::string target;
  std::string branch;
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
};

class ExecutionKernel {
 public:
  explicit ExecutionKernel(
      core::StateManager& stateManager,
      std::shared_ptr<ai::orchestration::IMultiModelOrchestrator>
          modelOrchestrator = nullptr);

  Result execute(const Action& action, const CognitiveState& state);
  Result executeIntent(const intent::Intent& intent,
                       const CognitiveState& state,
                       const governance::Policy& policy = {});

 private:
  [[nodiscard]] std::string stableActionId(const Action& action) const;
  void validateAction(const Action& action, const CognitiveState& state) const;
  void sortOutputs(Result& result) const;
  Result executeActionLocked(const Action& action, const CognitiveState& state);

  core::StateManager& stateManager_;
  std::shared_ptr<ai::orchestration::IMultiModelOrchestrator>
      modelOrchestrator_;
  std::mutex mutationQueueMutex_;
  std::uint64_t queueCounter_{0U};
};

}  // namespace ultra::runtime
