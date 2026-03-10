#pragma once

#include "StructuredResponse.h"
#include <string>

namespace ultra::api {

/// Common interface for unifying querying against different external tools.
class IApiConnector {
 public:
  virtual ~IApiConnector() = default;

  /// Check if the connector is properly configured (e.g. valid tokens).
  virtual bool isConfigured() const = 0;

  /// Retrieve a structured query from the connector.
  virtual StructuredResponse query(const std::string& requestPath) const = 0;
  
  /// Configure the connector dynamically with required auth tokens or config strings.
  virtual void configure(const std::string& configStr) = 0;

  /// Get the name identifier for the connector mapping (e.g., "github").
  virtual std::string getName() const = 0;
};

}  // namespace ultra::api
