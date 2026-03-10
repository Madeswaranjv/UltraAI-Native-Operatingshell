#include "ApiRegistry.h"

namespace ultra::api {

void ApiRegistry::registerConnector(std::unique_ptr<IApiConnector> connector) {
  if (connector) {
    connectors_[connector->getName()] = std::move(connector);
  }
}

IApiConnector* ApiRegistry::getConnector(const std::string& name) const {
  auto it = connectors_.find(name);
  if (it != connectors_.end()) {
    return it->second.get();
  }
  return nullptr;
}

std::vector<std::string> ApiRegistry::listConnectors() const {
  std::vector<std::string> names;
  names.reserve(connectors_.size());
  for (const auto& [name, ptr] : connectors_) {
    names.push_back(name);
  }
  return names;
}

}  // namespace ultra::api
