#pragma once

#include <string>
#include <vector>

namespace ultra::runtime::cognitive::tools {

struct ToolDefinition {
  std::string name;
  std::string description;
  std::vector<std::string> input_params;
  std::string output_description;

  [[nodiscard]] bool is_valid() const noexcept;
};

[[nodiscard]] std::vector<ToolDefinition> defaultToolDefinitions();

}  // namespace ultra::runtime::cognitive::tools
