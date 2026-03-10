#pragma once

#include "IApiConnector.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::api {

/// Global registry for dynamically resolving and utilizing registered API connectors.
class ApiRegistry {
 public:
  /// Register a connector instance.
  void registerConnector(std::unique_ptr<IApiConnector> connector);

  /// Fetch a connector by name, returning nullptr if missing.
  IApiConnector* getConnector(const std::string& name) const;

  /// Return a list of all installed connector names.
  std::vector<std::string> listConnectors() const;

 private:
  std::unordered_map<std::string, std::unique_ptr<IApiConnector>> connectors_;
};

}  // namespace ultra::api
