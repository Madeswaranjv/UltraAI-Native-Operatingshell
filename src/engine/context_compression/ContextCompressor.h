#pragma once

#include "ContextHierarchyBuilder.h"
#include "ContextSummaryGenerator.h"

#include "../context/ContextTypes.h"
#include "../query/SymbolQueryEngine.h"
#include "../../runtime/GraphSnapshot.h"

namespace ultra::engine::context_compression {

class ContextCompressor {
 public:
  [[nodiscard]] context::ContextSlice compressContext(
      const runtime::GraphSnapshot& snapshot,
      context::ContextSlice slice,
      std::size_t tokenBudget) const;
  [[nodiscard]] context::ContextSlice compressContext(
      const runtime::GraphSnapshot& snapshot,
      const context::ContextPlan& plan,
      const query::SymbolQueryEngine& queryEngine,
      context::ContextSlice slice,
      std::size_t tokenBudget) const;

 private:
  ContextHierarchyBuilder hierarchyBuilder_;
  ContextSummaryGenerator summaryGenerator_;
};

}  // namespace ultra::engine::context_compression
