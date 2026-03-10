#include "ProjectScanner.h"
#include "../core/ConfigManager.h"
#include "utils/FileClassifier.h"
#include <algorithm>
#include <iterator>
#include<iostream>
//E:\Projects\Ultra\src\scanner\ProjectScanner.cpp
namespace ultra::scanner {

ProjectScanner::ProjectScanner(const ultra::core::ConfigManager& config)
    : m_config(config) {}

FileType ProjectScanner::detectType(const std::filesystem::path& path) const {
  const std::string ext = path.extension().string();
  const std::string filename = path.filename().string();

  // ---- C++ Semantic Files ----
  const auto& exts = m_config.supportedExtensions();
  if (!ext.empty()) {
    auto it = std::ranges::find(exts, ext);
    if (it != exts.end()) {
      if (ext == ".h" || ext == ".hpp")
        return FileType::Header;
      return FileType::Source;
    }
  }

  // ---- Build System Files ----
  if (filename == "CMakeLists.txt" ||
      ext == ".cmake" ||
      ext == ".mk")
    return FileType::Build;

  // ---- Configuration Files ----
  if (ext == ".json" ||
      ext == ".yaml" || ext == ".yml" ||
      ext == ".toml" ||
      ext == ".ini" ||
      ext == ".cfg" ||
      ext == ".conf")
    return FileType::Config;

  // ---- Documentation ----
  if (ext == ".md" ||
      ext == ".txt" ||
      ext == ".rst")
    return FileType::Documentation;

  // ---- Scripts ----
  if (ext == ".py" ||
      ext == ".sh" ||
      ext == ".ps1" ||
      ext == ".bat")
    return FileType::Script;

  return FileType::Other;
}

bool ProjectScanner::shouldIgnore(const std::filesystem::path& path) const {
  if (!path.has_filename())
    return false;

  const std::string name = path.filename().string();
  const auto& ignore = m_config.ignoreDirs();

  return std::ranges::find(ignore, name) != ignore.end();
}

std::vector<FileInfo> ProjectScanner::scan(const std::filesystem::path& root) {
  std::vector<FileInfo> results;

  try {
    for (auto it = std::filesystem::recursive_directory_iterator(
             root,
             std::filesystem::directory_options::skip_permission_denied);
         it != std::filesystem::recursive_directory_iterator(); ++it) {

      if (it->is_directory()) {
        if (shouldIgnore(it->path().filename())) {
          it.disable_recursion_pending();
        }
        continue;
      }

      if (!it->is_regular_file())
        continue;

      const std::filesystem::path& p = it->path();

      // Skip Ultra internal artifacts
      if (ultra::utils::isToolArtifact(p))
        continue;

      FileInfo info;
      info.path = p;
      info.type = detectType(p);

      try {
        info.size = std::filesystem::file_size(p);
      } catch (...) {
        info.size = 0;
      }
        std::cout << p << " type=" << static_cast<int>(info.type) << "\n";
      results.push_back(std::move(info));
    }

  } catch (const std::filesystem::filesystem_error&) {
    throw;
  }

  return results;
}

}  // namespace ultra::scanner