#include "AdapterFactory.h"
#include "CMakeAdapter.h"
#include "FallbackAdapter.h"
#include "ReactAdapter.h"
#include "RustAdapter.h"

namespace ultra::adapters {

std::unique_ptr<ProjectAdapter> createProjectAdapter(
    ultra::core::ProjectType type,
    const std::filesystem::path& rootPath) {
  switch (type) {
    case ultra::core::ProjectType::React:
      return std::make_unique<ReactAdapter>(rootPath);
    case ultra::core::ProjectType::CMake:
      return std::make_unique<CMakeAdapter>(rootPath);
    case ultra::core::ProjectType::Rust:
      return std::make_unique<RustAdapter>(rootPath);
    case ultra::core::ProjectType::Make:
    case ultra::core::ProjectType::Python:
    case ultra::core::ProjectType::Unknown:
    default:
      return std::make_unique<FallbackAdapter>(rootPath, type);
  }
}

}  // namespace ultra::adapters
