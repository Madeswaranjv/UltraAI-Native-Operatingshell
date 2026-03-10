#pragma once

#include "../types/StructuredError.h"

#include <string>

namespace ultra::core {

/// Classifies raw error strings into StructuredError objects.
///
/// The classifier uses pattern matching to detect well-known error types
/// from compiler, linker, and tool output and assigns severity scores
/// and suggested actions.
class ErrorClassifier {
 public:
  /// Classify a raw error message string into a StructuredError.
  /// If the message cannot be classified, returns a generic error.
  static ultra::types::StructuredError classify(const std::string& rawMessage);

  /// Classify with additional context (category hint).
  static ultra::types::StructuredError classify(const std::string& rawMessage,
                                                const std::string& category);

  /// Classify a build tool error (compiler/linker output).
  static ultra::types::StructuredError classifyBuildError(
      const std::string& rawOutput);
};

}  // namespace ultra::core
