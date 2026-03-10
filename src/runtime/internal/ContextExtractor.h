#pragma once

// INTERNAL — DO NOT EXPOSE OUTSIDE KERNEL

#include "../CognitiveState.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ultra::runtime {

using SymbolID = std::uint64_t;

enum class QueryKind {
  Auto = 0,
  Symbol = 1,
  File = 2,
  Impact = 3
};

struct Query {
  QueryKind kind{QueryKind::Auto};
  std::string target;
  std::size_t impactDepth{2U};
};

struct ContextSlice {
  std::vector<SymbolID> includedNodes;
  std::string json;
  std::size_t estimatedTokens{0U};
};

class ContextExtractor {
 public:
  [[nodiscard]] ContextSlice getMinimalContext(
      const CognitiveState& state,
      const Query& query) const;
};

}  // namespace ultra::runtime
