#pragma once

#include "GraphTraversal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ultra::engine::query {

class QueryPlanner {
 public:
  [[nodiscard]] GraphTraversal::Plan<std::uint32_t> planFileDependencyQuery(
      const std::optional<std::uint32_t>& fileId) const;

  [[nodiscard]] GraphTraversal::Plan<std::uint64_t> planSymbolDependencyQuery(
      const std::vector<std::uint64_t>& symbolIds) const;

  [[nodiscard]] GraphTraversal::Plan<std::uint32_t> planImpactRegionQuery(
      const std::vector<std::uint32_t>& referenceFileIds,
      std::size_t maxDepth) const;

  template <typename T>
  static void sortAndDedupe(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
  }
};

}  // namespace ultra::engine::query

