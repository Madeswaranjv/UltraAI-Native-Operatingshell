#pragma once

#include "../model/ModelRequest.h"
#include "../model/ModelResponse.h"
#include "OrchestrationContext.h"

namespace ultra::ai::orchestration {

class IMultiModelOrchestrator {
 public:
  virtual ~IMultiModelOrchestrator() = default;

  [[nodiscard]] virtual model::ModelResponse generate(
      const model::ModelRequest& request,
      const OrchestrationContext& context) = 0;
};

}  // namespace ultra::ai::orchestration
