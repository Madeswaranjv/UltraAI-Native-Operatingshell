#pragma once

#include <filesystem>

namespace ultra::utils {

/** True if path is a tool-generated artifact (never hash, never in graph/context). */
bool isToolArtifact(const std::filesystem::path& p);

/** True if path is a source file for context and dependency graph (.cpp, .h, .hpp). */
bool isContextSourceFile(const std::filesystem::path& p);

/** True if path has an extension used for parsing includes (.cpp, .h, .hpp). */
bool isParsableSourceOrHeader(const std::filesystem::path& p);

}  // namespace ultra::utils
