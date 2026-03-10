#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::patch {

struct PatchOperation {
  std::string targetFile;
  std::vector<std::string> removedLines;
  std::vector<std::string> addedLines;
};

class DiffParser {
 public:
  static std::vector<PatchOperation> parse(
      const std::filesystem::path& diffFile);
};

}  // namespace ultra::patch
