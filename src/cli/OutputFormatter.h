#pragma once

#include "../types/StructuredOutput.h"

#include <iostream>
#include <string>

namespace ultra::cli {

/// Formats StructuredOutput for CLI display.
///
/// In human mode: prints readable text with colors/tags.
/// In JSON mode: prints the StructuredOutput as JSON to stdout.
class OutputFormatter {
 public:
  /// Print a StructuredOutput to stdout.
  /// If jsonMode is true, prints compact JSON.
  /// If jsonMode is false, prints human-readable format.
  static void print(const ultra::types::StructuredOutput& output,
                    bool jsonMode);

  /// Print just the data portion of a StructuredOutput (for legacy commands).
  static void printData(const nlohmann::json& data, bool jsonMode);

  /// Build a StructuredOutput for a successful command.
  static ultra::types::StructuredOutput success(
      const std::string& command, const nlohmann::json& data = {},
      const nlohmann::json& metadata = {});

  /// Build a StructuredOutput for a failed command.
  static ultra::types::StructuredOutput failure(
      const std::string& command,
      const std::vector<ultra::types::StructuredError>& errors,
      const nlohmann::json& data = {},
      const nlohmann::json& metadata = {});

  /// Build a StructuredOutput from current Logger error state.
  static ultra::types::StructuredOutput fromLoggerState(
      const std::string& command, const nlohmann::json& data = {},
      const nlohmann::json& metadata = {});
};

}  // namespace ultra::cli
