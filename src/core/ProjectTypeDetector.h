#pragma once

#include "ProjectType.h"
#include <filesystem>
#include <optional>
#include <string>

namespace ultra::core {

class ProjectTypeDetector {
 public:
  static ProjectType detect(const std::filesystem::path& root);
  static std::optional<ProjectType> fromString(const std::string& value);
  static std::string toString(ProjectType type);

 private:
  static bool hasFile(const std::filesystem::path& root,
                      const char* fileName) noexcept;
};

}  // namespace ultra::core
