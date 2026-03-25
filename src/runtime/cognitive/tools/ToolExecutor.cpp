#include "ToolExecutor.h"

#include <map>
#include <memory>
#include <string>

namespace ultra::runtime::cognitive::tools {

namespace {

bool startsWithError(const std::string& value) {
  return value.rfind("ERROR:", 0U) == 0U || value.rfind("[ERROR]", 0U) == 0U;
}

}  // namespace

ToolExecutor::ToolExecutor()
    : registry_(),
      router_(std::make_shared<tool_router::ToolRouter>()) {}

ToolExecutor::ToolExecutor(std::shared_ptr<tool_router::ToolRouter> router,
                           ToolRegistry registry)
    : registry_(std::move(registry)),
      router_(std::move(router)) {
  if (router_ == nullptr) {
    router_ = std::make_shared<tool_router::ToolRouter>();
  }
}

std::string ToolExecutor::execute(
    const std::string& tool_name,
    const std::map<std::string, std::string>& args) {
  lastExecutionSucceeded_ = false;

  const ToolDefinition* tool = registry_.get_tool(tool_name);
  if (tool == nullptr) {
    ++consecutiveFailures_[tool_name];
    return "ERROR: tool '" + tool_name + "' is not registered.";
  }

  if (router_ == nullptr) {
    ++consecutiveFailures_[tool_name];
    return "ERROR: tool router is unavailable.";
  }

  for (const std::string& required : tool->input_params) {
    const auto it = args.find(required);
    if (it == args.end() || it->second.empty()) {
      ++consecutiveFailures_[tool_name];
      return "ERROR: missing required argument '" + required +
             "' for tool '" + tool_name + "'.";
    }
  }

  const std::string output = router_->route_and_execute(tool_name, args);
  if (startsWithError(output)) {
    ++consecutiveFailures_[tool_name];
    return output;
  }

  lastExecutionSucceeded_ = true;
  consecutiveFailures_[tool_name] = 0U;
  return output;
}

const ToolRegistry& ToolExecutor::registry() const noexcept {
  return registry_;
}

bool ToolExecutor::has_router() const noexcept {
  return router_ != nullptr;
}

bool ToolExecutor::last_execution_succeeded() const noexcept {
  return lastExecutionSucceeded_;
}

std::size_t ToolExecutor::consecutive_failures(
    const std::string& tool_name) const noexcept {
  const auto it = consecutiveFailures_.find(tool_name);
  if (it == consecutiveFailures_.end()) {
    return 0U;
  }
  return it->second;
}

}  // namespace ultra::runtime::cognitive::tools
