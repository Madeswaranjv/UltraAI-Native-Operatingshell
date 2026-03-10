#pragma once

#include "ContextTypes.h"

#include <string>

namespace ultra::engine::context {

class ContextPlanner {
 public:
  [[nodiscard]] ContextPlan planSymbolContext(
      const ContextRequest& request,
      const query::SymbolQueryEngine& queryEngine) const;
  [[nodiscard]] ContextPlan planFileContext(
      const ContextRequest& request,
      const query::SymbolQueryEngine& queryEngine,
      const ai::RuntimeState& state,
      const std::string& resolvedFilePath) const;
  [[nodiscard]] ContextPlan planImpactContext(
      const ContextRequest& request,
      const query::SymbolQueryEngine& queryEngine) const;
};

}  // namespace ultra::engine::context
