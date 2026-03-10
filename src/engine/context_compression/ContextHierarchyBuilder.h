#pragma once

#include "../context/ContextTypes.h"
#include "../../runtime/GraphSnapshot.h"

#include <external/json.hpp>

namespace ultra::engine::context_compression {

class ContextHierarchyBuilder {
 public:
  [[nodiscard]] nlohmann::ordered_json buildHierarchy(
      const runtime::GraphSnapshot& snapshot,
      const context::ContextSlice& slice) const;
};

}  // namespace ultra::engine::context_compression
