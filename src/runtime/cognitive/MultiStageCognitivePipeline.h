#pragma once

#include "CognitiveRuntime.h"

#include "../../ai/orchestration/IMultiModelOrchestrator.h"
#include "../governance/Policy.h"
#include "../intent/Intent.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ultra::core {
class StateManager;
}

namespace ultra::runtime::cognitive {

struct MultiStagePipelineRequest {
  std::string rawPrompt;
  std::string actionLabel;
  std::string requestedRole;
  std::vector<std::string> targets;
  std::vector<std::string> constraints;
  bool requiresPlanning{true};
  bool autoCommit{false};
  intent::Intent resolvedIntent;
  governance::Policy policy;
};

class MultiStageCognitivePipeline {
 public:
  explicit MultiStageCognitivePipeline(
      core::StateManager& stateManager,
      std::shared_ptr<ai::orchestration::IMultiModelOrchestrator> orchestrator =
          nullptr);

  [[nodiscard]] CognitiveLoopResult run(const MultiStagePipelineRequest& request);

 private:
  core::StateManager& stateManager_;
  std::shared_ptr<ai::orchestration::IMultiModelOrchestrator> orchestrator_;
};

}  // namespace ultra::runtime::cognitive
