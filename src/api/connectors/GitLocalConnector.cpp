#include "GitLocalConnector.h"
#include "../../core/Logger.h"

namespace ultra::api::connectors {

bool GitLocalConnector::isConfigured() const {
  return true; // Local git doesn't strictly need a configured token
}

void GitLocalConnector::configure(const std::string& /*configStr*/) {
  // Can be used to pass a path to the git workspace, default is cwd.
}

std::string GitLocalConnector::getName() const {
  return "git";
}

StructuredResponse GitLocalConnector::query(const std::string& requestPath) const {
  StructuredResponse res;
  res.source = getName();
  res.timestamp = ultra::types::Timestamp::now();
  
  ultra::core::Logger::info(ultra::core::LogCategory::General, 
      "Executing structured local git query: " + requestPath);

  // MOCK IMPLEMENTATION: A full implementation would utilize `ProcessExecutor`
  // mapped to `git status --porcelain` mapped into structured arrays.
  
  if (requestPath.find("diff") != std::string::npos) {
    res.changeType = "file_delta";
    res.data = nlohmann::json::array({
      { {"file", "src/api/IApiConnector.h"}, {"status", "added"}, {"lines_added", 25} }
    });
    res.impactScore = 0.2;
    res.success = true;
  } else if (requestPath.find("log") != std::string::npos) {
    res.changeType = "commit";
    res.data = nlohmann::json::array({
      { {"hash", "a1b2c3d4"}, {"message", "Initial API framework commit"} }
    });
    res.impactScore = 0.5;
    res.success = true;
  } else {
    res.errorMessage = "Unknown git endpoint mapping for: " + requestPath;
    res.success = false;
  }

  return res;
}

}  // namespace ultra::api::connectors
