#include "AdapterFactory.h"
#include "CppAdapter.h"
#include <filesystem>
//E:\Projects\Ultra\src\language\AdapterFactory.cpp
namespace ultra::language {

namespace {

bool looksLikeCppProject(const std::filesystem::path& root) {
  if (std::filesystem::exists(root / "CMakeLists.txt")) return true;
  try {
    for (auto it = std::filesystem::recursive_directory_iterator(root);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
      if (it->is_regular_file() && it->path().extension() == ".cpp")
        return true;
    }
  } catch (...) {
  }
  return false;
}

}  // namespace

std::unique_ptr<ILanguageAdapter> AdapterFactory::create(
    const std::filesystem::path& root) {
  if (looksLikeCppProject(root)) {
    return std::make_unique<CppAdapter>();
  }
  return std::make_unique<CppAdapter>();
}

}  // namespace ultra::language
