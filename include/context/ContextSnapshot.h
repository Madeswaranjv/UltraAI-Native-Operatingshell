#pragma once

#include <filesystem>
#include <unordered_map>
#include <string>

namespace ultra::context {

/** Load path->hash snapshot from file (line-based path|hash). Returns empty on error. */
std::unordered_map<std::string, std::string> loadSnapshot(
    const std::filesystem::path& path);

/** Save path->hash snapshot to file. */
void saveSnapshot(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, std::string>& snapshot);

}  // namespace ultra::context
