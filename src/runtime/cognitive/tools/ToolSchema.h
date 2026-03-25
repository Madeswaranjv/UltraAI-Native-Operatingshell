#pragma once

#include <map>
#include <string>

namespace ultra::runtime::cognitive::tools {

struct ToolSchema {
  std::string name;
  std::map<std::string, std::string> parameters;
};

}  // namespace ultra::runtime::cognitive::tools

