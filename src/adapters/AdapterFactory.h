#pragma once

#include "ProjectAdapter.h"
#include "../core/ProjectType.h"
#include <filesystem>
#include <memory>

namespace ultra::adapters {

std::unique_ptr<ProjectAdapter> createProjectAdapter(
    ultra::core::ProjectType type,
    const std::filesystem::path& rootPath);

}  // namespace ultra::adapters
