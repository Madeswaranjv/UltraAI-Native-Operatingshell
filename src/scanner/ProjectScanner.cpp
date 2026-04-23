#include "ProjectScanner.h"
#include "../ai/FileRegistry.h"
#include "../core/ConfigManager.h"
#include "utils/FileClassifier.h"

#include <algorithm>
#include <iterator>
#include <iostream>
#include <chrono>

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

  if (ultra::ai::FileRegistry::shouldIgnoreDirectory(path)) {
    return true;
  }

  const std::string name = path.filename().string();
  const auto& ignore = m_config.ignoreDirs();

  return std::ranges::find(ignore, name) != ignore.end();
}

std::vector<FileInfo> ProjectScanner::scan(const std::filesystem::path& root) {

  using clock = std::chrono::high_resolution_clock;
  auto start = clock::now();

  std::vector<FileInfo> results;

  try {

    for (auto it = std::filesystem::recursive_directory_iterator(
             root,
             std::filesystem::directory_options::skip_permission_denied);
         it != std::filesystem::recursive_directory_iterator(); ++it) {

      const std::filesystem::path& path = it->path();

      // ---- Skip heavy directories ----
      if (it->is_directory()) {
        if (shouldIgnore(path.filename())) {
          it.disable_recursion_pending();
          continue;
        }

        continue;
      }

      if (!it->is_regular_file())
        continue;

      // Skip Ultra internal artifacts
      if (ultra::utils::isToolArtifact(path))
        continue;

      FileInfo info;

      info.path = path;
      info.type = detectType(path);

      try {
        info.size = std::filesystem::file_size(path);
      } catch (...) {
        info.size = 0;
      }

      results.push_back(std::move(info));
    }

  } catch (const std::filesystem::filesystem_error&) {
    throw;
  }

  auto end = clock::now();

  double seconds =
      std::chrono::duration<double>(end - start).count();

  std::cout << "[scanner] scanned "
            << results.size()
            << " files in "
            << std::fixed
            << std::setprecision(2)
            << seconds
            << " seconds\n";

  return results;
}

}  // namespace ultra::scanner
