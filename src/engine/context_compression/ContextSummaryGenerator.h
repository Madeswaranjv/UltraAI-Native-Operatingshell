#pragma once

#include "../context/ContextTypes.h"
#include "../query/SymbolQueryEngine.h"
#include "../../runtime/GraphSnapshot.h"

#include <external/json.hpp>

#include <cstddef>

namespace ultra::engine::context_compression {

class ContextSummaryGenerator {
 public:
  [[nodiscard]] nlohmann::ordered_json generateSummary(
      const runtime::GraphSnapshot& snapshot,
      const context::ContextPlan& plan,
      const query::SymbolQueryEngine& queryEngine,
      const nlohmann::ordered_json& hierarchy,
      const context::ContextSlice& slice,
      std::size_t tokenBudget) const;
};

}  // namespace ultra::engine::context_compression
