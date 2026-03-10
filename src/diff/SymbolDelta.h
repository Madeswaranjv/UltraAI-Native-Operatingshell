#pragma once

#include "../types/Delta.h"
#include "../ai/SymbolTable.h"

#include <string>

namespace ultra::diff {

/// Represents a change to a single symbol across two snapshots.
struct SymbolDelta {
  /// Name of the symbol (e.g., class name, function name).
  std::string symbolName;

  /// The type of change (Added, Removed, Modified, Renamed).
  ultra::types::ChangeType changeType{ultra::types::ChangeType::Unchanged};

  /// The old signature/record before the change.
  ultra::ai::SymbolRecord oldRecord;

  /// The new signature/record after the change.
  ultra::ai::SymbolRecord newRecord;

  /// Confidence score that this change was correctly detected.
  double confidence{1.0};
};

}  // namespace ultra::diff
