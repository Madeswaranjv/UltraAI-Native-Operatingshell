#pragma once

#include "../tool_router/ToolRouter.h"
#include "ToolRegistry.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>

namespace ultra::runtime::cognitive::tools {

class ToolExecutor {
 public:
  ToolExecutor();
  explicit ToolExecutor(
      std::shared_ptr<tool_router::ToolRouter> router,
      ToolRegistry registry = ToolRegistry{});

  std::string execute(const std::string& tool_name,
                      const std::map<std::string, std::string>& args);

  [[nodiscard]] const ToolRegistry& registry() const noexcept;
  [[nodiscard]] bool has_router() const noexcept;
  [[nodiscard]] bool last_execution_succeeded() const noexcept;
  [[nodiscard]] std::size_t consecutive_failures(
      const std::string& tool_name) const noexcept;

 private:
  ToolRegistry registry_;
  std::shared_ptr<tool_router::ToolRouter> router_;
  bool lastExecutionSucceeded_{false};
  std::map<std::string, std::size_t> consecutiveFailures_;
};

}  // namespace ultra::runtime::cognitive::tools
