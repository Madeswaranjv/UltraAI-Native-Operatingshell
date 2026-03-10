#pragma once

#include "../FileRegistry.h"

#include <filesystem>

namespace ultra::ai::parsing {

Language detectLanguage(const std::filesystem::path& path);

}  // namespace ultra::ai::parsing
