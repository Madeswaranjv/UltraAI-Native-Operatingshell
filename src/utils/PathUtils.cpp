#include "PathUtils.h"

namespace ultra::utils {

std::string toNormalizedString(const std::filesystem::path& path) {
  return path.lexically_normal().string();
}

std::filesystem::path resolvePath(const std::string& pathStr) {
  std::filesystem::path p(pathStr);
  return std::filesystem::absolute(p).lexically_normal();
}

bool pathExists(const std::filesystem::path& path) {
  return std::filesystem::exists(path);
}

bool isDirectory(const std::filesystem::path& path) {
  return std::filesystem::is_directory(path);
}

}  // namespace ultra::utils
