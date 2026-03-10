#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::graph {

class IncludeParser {
 public:
  static std::vector<std::string> extractIncludes(
      const std::filesystem::path& file);
};

}  // namespace ultra::graph
