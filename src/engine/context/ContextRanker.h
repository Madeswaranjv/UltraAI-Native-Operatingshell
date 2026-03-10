#pragma once

#include "ContextTypes.h"

#include <vector>

namespace ultra::engine::context {

class ContextRanker {
 public:
  [[nodiscard]] std::vector<RankedSymbolCandidate> rankSymbols(
      const std::vector<SymbolContextCandidate>& candidates,
      const ContextRequest& request) const;
  [[nodiscard]] std::vector<RankedFileCandidate> rankFiles(
      const std::vector<FileContextCandidate>& candidates,
      const ContextRequest& request) const;
};

}  // namespace ultra::engine::context
