#pragma once

#include "../IApiConnector.h"
#include <string>

namespace ultra::api::connectors {

/// Communicates with the GitHub REST API to fetch repo, issue, and PR data.
class GitHubConnector : public IApiConnector {
 public:
  GitHubConnector() = default;

  bool isConfigured() const override;
  void configure(const std::string& configStr) override;
  StructuredResponse query(const std::string& requestPath) const override;
  std::string getName() const override;

 private:
  std::string token_;
};

}  // namespace ultra::api::connectors
