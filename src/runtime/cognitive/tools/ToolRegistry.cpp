#include "ToolRegistry.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ultra::runtime::cognitive::tools {

ToolRegistry::ToolRegistry() {
  for (const ToolDefinition& tool : defaultToolDefinitions()) {
    register_tool(tool);
  }
}

void ToolRegistry::register_tool(const ToolDefinition& tool) {
  if (!tool.is_valid()) {
    return;
  }
  tools_[tool.name] = tool;
}

const ToolDefinition* ToolRegistry::get_tool(const std::string& name) const {
  const auto it = tools_.find(name);
  if (it == tools_.end()) {
    return nullptr;
  }
  return &it->second;
}

std::vector<std::string> ToolRegistry::list_tools() const {
  std::vector<std::string> names;
  names.reserve(tools_.size());
  for (const auto& [name, tool] : tools_) {
    (void)tool;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace ultra::runtime::cognitive::tools

