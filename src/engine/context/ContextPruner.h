#pragma once

#include "ContextTypes.h"

#include <optional>
#include <vector>

namespace ultra::engine::context {

class ContextPruner {
 public:
  struct PruneDecision {
    enum class Kind : std::uint8_t { Symbol = 0U, File = 1U };

    Kind kind{Kind::Symbol};
    std::size_t index{0U};
  };

  [[nodiscard]] std::optional<PruneDecision> selectNextCandidate(
      const std::vector<RankedSymbolCandidate>& rankedSymbols,
      const std::vector<RankedFileCandidate>& rankedFiles) const;

  static void collapseStrings(std::vector<std::string>& values);
  static void collapseDefinitions(
      std::vector<query::SymbolDefinition>& definitions);
};

}  // namespace ultra::engine::context
