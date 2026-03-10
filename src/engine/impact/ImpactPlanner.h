#pragma once

#include "ImpactTypes.h"
#include "../context/ContextBuilder.h"
#include "../query/SymbolQueryEngine.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ultra::engine::impact {

class ImpactPlanner {
 public:
  [[nodiscard]] ImpactPlan planSymbolImpact(
      const std::string& symbolName,
      const query::SymbolQueryEngine& queryEngine,
      const context::ContextBuilder& contextBuilder,
      std::size_t maxDepth = 2U) const;

  [[nodiscard]] ImpactPlan planFileImpact(
      const std::string& filePath,
      const query::SymbolQueryEngine& queryEngine,
      const context::ContextBuilder& contextBuilder,
      std::size_t maxDepth = 2U) const;

 private:
  static std::size_t determineTraversalDepth(
      const context::ContextSlice& slice,
      std::size_t requestedDepth);
  static std::size_t determineBoundaryLimit(
      const context::ContextSlice& slice,
      const char* metadataKey,
      std::size_t minimumLimit,
      std::size_t multiplier);
  static std::vector<std::string> extractImpactRegion(
      const context::ContextSlice& slice);
  static std::vector<std::string> extractNodeNames(
      const context::ContextSlice& slice);
  static std::vector<std::string> extractNodeNamesForFile(
      const context::ContextSlice& slice,
      const std::string& filePath);
  static std::vector<std::string> extractRootFilePaths(
      const context::ContextSlice& slice,
      const std::string& symbolName);
};

}  // namespace ultra::engine::impact
