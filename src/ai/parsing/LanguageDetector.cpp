#include "LanguageDetector.h"

#include <string>

namespace ultra::ai::parsing {

Language detectLanguage(const std::filesystem::path& path) {
  const std::string ext = path.extension().string();
  if (ext == ".cpp" || ext == ".hpp" || ext == ".h") {
    return Language::Cpp;
  }
  if (ext == ".js") {
    return Language::JavaScript;
  }
  if (ext == ".ts") {
    return Language::TypeScript;
  }
  if (ext == ".py") {
    return Language::Python;
  }
  if (ext == ".java") {
    return Language::Java;
  }
  if (ext == ".go") {
    return Language::Go;
  }
  if (ext == ".rs") {
    return Language::Rust;
  }
  if (ext == ".cs") {
    return Language::CSharp;
  }
  return Language::Unknown;
}

}  // namespace ultra::ai::parsing
