#include "OutputFormatter.h"

#include "../core/Logger.h"
#include "../types/Serialization.h"

namespace ultra::cli {

void OutputFormatter::print(const ultra::types::StructuredOutput& output,
                            bool jsonMode) {
  if (jsonMode) {
    nlohmann::json j;
    ultra::types::to_json(j, output);
    std::cout << j.dump() << '\n';
    return;
  }

  // Human-readable mode: print status, then data, then errors.
  if (output.status == ultra::types::OutputStatus::Failure) {
    std::cerr << "[FAILED] " << output.command << '\n';
    for (const auto& err : output.errors) {
      std::cerr << "  [" << err.errorType << "] " << err.message << '\n';
      if (!err.suggestedAction.empty()) {
        std::cerr << "    Suggested: " << err.suggestedAction << '\n';
      }
    }
  } else if (output.status == ultra::types::OutputStatus::Success) {
    // Data is typically already printed by the command handler in human mode.
    // This is mainly used when the formatter owns the full output.
    if (!output.data.is_null() && !output.data.empty()) {
      if (output.data.is_string()) {
        std::cout << output.data.get<std::string>() << '\n';
      } else {
        std::cout << output.data.dump(2) << '\n';
      }
    }
  } else if (output.status == ultra::types::OutputStatus::Partial) {
    std::cout << "[PARTIAL] " << output.command << '\n';
  }
}

void OutputFormatter::printData(const nlohmann::json& data, bool jsonMode) {
  if (jsonMode) {
    std::cout << data.dump() << '\n';
  } else {
    if (data.is_string()) {
      std::cout << data.get<std::string>() << '\n';
    } else if (!data.is_null()) {
      std::cout << data.dump(2) << '\n';
    }
  }
}

ultra::types::StructuredOutput OutputFormatter::success(
    const std::string& command, const nlohmann::json& data,
    const nlohmann::json& metadata) {
  ultra::types::StructuredOutput out;
  out.status = ultra::types::OutputStatus::Success;
  out.command = command;
  out.data = data;
  out.metadata = metadata;
  out.timestamp = ultra::types::Timestamp::now();
  return out;
}

ultra::types::StructuredOutput OutputFormatter::failure(
    const std::string& command,
    const std::vector<ultra::types::StructuredError>& errors,
    const nlohmann::json& data,
    const nlohmann::json& metadata) {
  ultra::types::StructuredOutput out;
  out.status = ultra::types::OutputStatus::Failure;
  out.command = command;
  out.data = data;
  out.errors = errors;
  out.metadata = metadata;
  out.timestamp = ultra::types::Timestamp::now();
  return out;
}

ultra::types::StructuredOutput OutputFormatter::fromLoggerState(
    const std::string& command, const nlohmann::json& data,
    const nlohmann::json& metadata) {
  const auto& errors = ultra::core::Logger::collectedErrors();
  if (errors.empty()) {
    return success(command, data, metadata);
  }
  return failure(command, errors, data, metadata);
}

}  // namespace ultra::cli
