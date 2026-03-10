#pragma once

#include <filesystem>
#include <string>

namespace ultra::utils {

std::string toNormalizedString(const std::filesystem::path& path);
std::filesystem::path resolvePath(const std::string& pathStr);
bool pathExists(const std::filesystem::path& path);
bool isDirectory(const std::filesystem::path& path);

}  // namespace ultra::utils
