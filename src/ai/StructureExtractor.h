#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

class StructureExtractor {
 public:
  static std::vector<std::string> extractClasses(
      const std::filesystem::path& file);
  static std::vector<std::string> extractStructs(
      const std::filesystem::path& file);
  static std::vector<std::string> extractFunctions(
      const std::filesystem::path& file);
};

}  // namespace ultra::ai
