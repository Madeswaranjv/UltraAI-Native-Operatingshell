#pragma once

#include <string>
#include <vector>
#include <memory>
#include <external/json.hpp>

namespace ultra::sdk {

/// Exposes the Ultra cognitive shell as a programmable C++ library.
class UltraRuntime {
 public:
  UltraRuntime();
  ~UltraRuntime();

  /// Initialize the runtime targeting a specific project workspace.
  bool init(const std::string& projectRoot);

  /// Synchronously execute an Ultra CLI command string.
  /// Returns a structured JSON result payload.
  nlohmann::json execute(const std::string& command, const std::vector<std::string>& args);

  /// Ask the agent to perform goal-directed reasoning.
  nlohmann::json think(const std::string& goal);

 private:
  struct Impl;
  std::unique_ptr<Impl> pimpl_;
};

}  // namespace ultra::sdk
