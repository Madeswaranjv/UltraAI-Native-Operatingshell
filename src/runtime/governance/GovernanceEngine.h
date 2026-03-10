#pragma once

#include "../CognitiveState.h"
#include "../intent/Strategy.h"
#include "../../policy_evolution/AdaptivePolicyEngine.h"
#include "GovernanceReport.h"
#include "Policy.h"

#include <mutex>

namespace ultra::memory {
class CognitiveMemoryManager;
}

namespace ultra::runtime::governance {

class GovernanceEngine {
 public:
  explicit GovernanceEngine(memory::CognitiveMemoryManager* memoryManager = nullptr);

  void bindMemoryManager(memory::CognitiveMemoryManager* memoryManager);

  [[nodiscard]] GovernanceReport evaluate(
      const intent::Strategy& strategy,
      const Policy& policy,
      const CognitiveState& state) const;

 private:
  mutable std::mutex memoryMutex_;
  memory::CognitiveMemoryManager* memoryManager_{nullptr};
  mutable policy_evolution::AdaptivePolicyEngine adaptiveEngine_{};
};

}  // namespace ultra::runtime::governance
