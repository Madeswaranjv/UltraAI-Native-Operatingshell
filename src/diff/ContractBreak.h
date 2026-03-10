#pragma once

#include <string>
#include <vector>

namespace ultra::diff {

/// The type of API contract break detected.
enum class BreakType {
  Unknown = 0,
  ParameterChange = 1,
  ReturnChange = 2,
  VisibilityChange = 3,
  Removed = 4,
  TypeChange = 5
};

/// Represents a detected breaking change in an API contract.
struct ContractBreak {
  /// Name of the affected symbol (e.g., function or class name).
  std::string functionName;

  /// The specific type of break.
  BreakType breakType{BreakType::Unknown};

  /// Severity score (0.0 to 1.0) indicating how breaking this is.
  double severity{1.0};

  /// Description of the break.
  std::string description;

  /// List of downstream callers affected by this break.
  std::vector<std::string> affectedCallers;
};

}  // namespace ultra::diff
