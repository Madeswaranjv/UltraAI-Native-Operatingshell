#pragma once

#include <external/json.hpp>
#include <string>
#include "../types/Timestamp.h"

namespace ultra::api {

/// Represents a standardized payload returning from any external system connector.
struct StructuredResponse {
  std::string source;      // e.g., "github", "jira", "git"
  std::string changeType;  // e.g., "pull_request", "commit", "issue"
  nlohmann::json data;     // Structured API payload delta
  ultra::types::Timestamp timestamp;
  double impactScore{0.0}; // Quantified severity/impact from the external tool
  bool success{false};
  std::string errorMessage;
};

}  // namespace ultra::api
