#include "UltraRuntime.h"
#include "../core/Logger.h"

namespace ultra::sdk {

struct UltraRuntime::Impl {
  std::string workspaceRoot;
  bool isInitialized{false};
};

UltraRuntime::UltraRuntime() : pimpl_(std::make_unique<Impl>()) {}
UltraRuntime::~UltraRuntime() = default;

bool UltraRuntime::init(const std::string& projectRoot) {
  pimpl_->workspaceRoot = projectRoot;
  pimpl_->isInitialized = true;
  ultra::core::Logger::info(ultra::core::LogCategory::General, 
      "Embedded Ultra SDK initialized in workspace: " + projectRoot);
  return true;
}

nlohmann::json UltraRuntime::execute(const std::string& command, const std::vector<std::string>& /*args*/) {
  if (!pimpl_->isInitialized) {
    return {{"error", "Runtime not initialized"}};
  }
  
  // Real implementation would route strings directly through the CLIEngine core
  // returning captured nlohmann::json payloads instead of stdout.
  
  return {
    {"status", "success"},
    {"command_executed", command},
    {"mock_response", "Service command executed locally."}
  };
}

nlohmann::json UltraRuntime::think(const std::string& goal) {
  if (!pimpl_->isInitialized) {
    return {{"error", "Runtime not initialized"}};
  }

  // Real implementation links into ultra::orchestration::IntentDecomposer
  
  return {
    {"status", "success"},
    {"goal", goal},
    {"confidence", 0.95},
    {"branch_id", "br-12345"}
  };
}

}  // namespace ultra::sdk
