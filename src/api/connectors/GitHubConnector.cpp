#include "GitHubConnector.h"
#include "../../core/Logger.h"

namespace ultra::api::connectors {

bool GitHubConnector::isConfigured() const {
  return !token_.empty();
}

void GitHubConnector::configure(const std::string& configStr) {
  // Simple extraction: assume configStr == token for now,
  // or parse JSON if complex config is needed.
  token_ = configStr;
}

std::string GitHubConnector::getName() const {
  return "github";
}

StructuredResponse GitHubConnector::query(const std::string& requestPath) const {
  StructuredResponse res;
  res.source = getName();
  res.timestamp = ultra::types::Timestamp::now();
  
  if (!isConfigured()) {
    res.errorMessage = "GitHub API connector is not configured (missing token).";
    res.success = false;
    return res;
  }

  // MOCK IMPLEMENTATION: A real implementation would use libcurl or cpp-httplib
  // to fetch data from https://api.github.com/...
  // For Phase 6 architectural setup, we mock structured returns.

  ultra::core::Logger::info(ultra::core::LogCategory::General, 
      "Mock API Call to GitHub: " + requestPath);

  if (requestPath.find("prs") != std::string::npos) {
    res.changeType = "pull_request";
    res.data = nlohmann::json::array({
      { {"id", 101}, {"title", "Update orchestration subsystem"}, {"author", "octocat"} },
      { {"id", 102}, {"title", "Fix relevance calibration math"}, {"author", "hubot"} }
    });
    res.impactScore = 0.8;
    res.success = true;
  } else if (requestPath.find("issues") != std::string::npos) {
    res.changeType = "issue";
    res.data = nlohmann::json::array({
      { {"id", 42}, {"title", "Branch merge conflicts occasionally crash cache"}, {"state", "open"} }
    });
    res.impactScore = 0.6;
    res.success = true;
  } else {
    res.errorMessage = "Unknown GitHub endpoint mapping for: " + requestPath;
    res.success = false;
  }

  return res;
}

}  // namespace ultra::api::connectors
