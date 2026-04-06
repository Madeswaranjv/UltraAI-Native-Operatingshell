#pragma once

#include "task_graph.h"

#include <string>
#include <vector>

namespace ultra::runtime::cognitive {

struct MicroPlanInput {
  std::string intentId;
  std::string strategyId;
  std::string planId;
  std::vector<TaskPayload> taskPayloads;
  ::ultra::runtime::intent::IntentMemoryContext memory;
};

class MicroPlanner {
 public:
  [[nodiscard]] TaskGraph generate_plan(const MicroPlanInput& input) const;
};

}  // namespace ultra::runtime::cognitive