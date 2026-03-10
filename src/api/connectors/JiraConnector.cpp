#include "JiraConnector.h"
#include "../../core/Logger.h"

namespace ultra::api::connectors {

bool JiraConnector::isConfigured() const {
  return !credentials_.empty();
}

void JiraConnector::configure(const std::string& configStr) {
  credentials_ = configStr;
}

std::string JiraConnector::getName() const {
  return "jira";
}

StructuredResponse JiraConnector::query(const std::string& requestPath) const {
  StructuredResponse res;
  res.source = getName();
  res.timestamp = ultra::types::Timestamp::now();
  
  if (!isConfigured()) {
    res.errorMessage = "Jira API requires base64 auth config.";
    res.success = false;
    return res;
  }

  // MOCK IMPLEMENTATION
  ultra::core::Logger::info(ultra::core::LogCategory::General, 
      "Mock API Call to Jira: " + requestPath);

  res.changeType = "ticket";
  res.data = nlohmann::json::object({
    {"id", "ULTRA-301"},
    {"summary", "Resolve race conditions on parallel execution DAG"},
    {"status", "In Progress"}
  });
  res.impactScore = 0.9; // critical issue
  res.success = true;

  return res;
}

}  // namespace ultra::api::connectors
