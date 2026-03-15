#include "ast/AstExtractor.h"

namespace ultra::ast {

bool AstExtractor::isLibClangAvailable() {
  return false;
}

FileStructure AstExtractor::extract(const std::filesystem::path& file) {
  (void)file;
  return FileStructure{};
}

}  // namespace ultra::ast
