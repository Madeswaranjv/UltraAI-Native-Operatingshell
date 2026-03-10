#pragma once

#include "../IApiConnector.h"
#include <string>

namespace ultra::api::connectors {

/// Wrap raw git CLI commands via ultra::core::ProcessExecutor and map outputs to structured JSON.
class GitLocalConnector : public IApiConnector {
 public:
  GitLocalConnector() = default;

  bool isConfigured() const override;
  void configure(const std::string& configStr) override;
  StructuredResponse query(const std::string& requestPath) const override;
  std::string getName() const override;
};

}  // namespace ultra::api::connectors
