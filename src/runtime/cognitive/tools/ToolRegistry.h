#pragma once

#include "ToolDefinition.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::runtime::cognitive::tools {

class ToolRegistry {
 public:
  ToolRegistry();

  void register_tool(const ToolDefinition& tool);
  [[nodiscard]] const ToolDefinition* get_tool(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> list_tools() const;

 private:
  std::unordered_map<std::string, ToolDefinition> tools_;
};

}  // namespace ultra::runtime::cognitive::tools

