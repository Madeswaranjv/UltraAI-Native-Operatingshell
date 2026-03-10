#pragma once
//E:\Projects\Ultra\src\scanner\ProjectScanner.h
#include "FileInfo.h"
#include <filesystem>
#include <vector>

namespace ultra::core {
class ConfigManager;
}

namespace ultra::scanner {

class ProjectScanner {
 public:
  explicit ProjectScanner(const ultra::core::ConfigManager& config);

  std::vector<FileInfo> scan(const std::filesystem::path& root);
  FileType detectType(const std::filesystem::path& path) const;
  bool shouldIgnore(const std::filesystem::path& path) const;

 private:
  const ultra::core::ConfigManager& m_config;
};

}  // namespace ultra::scanner