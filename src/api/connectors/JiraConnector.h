#pragma once

#include "../IApiConnector.h"
#include <string>

namespace ultra::api::connectors {

/// Communicates with Jira REST API to fetch ticket and sprint tracker data.
class JiraConnector : public IApiConnector {
 public:
  JiraConnector() = default;

  bool isConfigured() const override;
  void configure(const std::string& configStr) override;
  StructuredResponse query(const std::string& requestPath) const override;
  std::string getName() const override;

 private:
  std::string credentials_;
};

}  // namespace ultra::api::connectors
