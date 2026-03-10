#pragma once

#include "SymbolDelta.h"
#include "../ai/SymbolTable.h"
//SymbolDiff.h
#include <vector>

namespace ultra::diff {

/// Engine for computing symbol-level structural diffs.
class SymbolDiff {
 public:
  /// Compare two sets of symbol records and compute the differences.
  /// Identifies added, removed, and modified symbols.
  /// (Renamed detection is a future enhancement).
  static std::vector<SymbolDelta> compare(
      const std::vector<ultra::ai::SymbolRecord>& oldSymbols,
      const std::vector<ultra::ai::SymbolRecord>& newSymbols);
};

}  // namespace ultra::diff
