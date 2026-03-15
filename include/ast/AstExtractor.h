#pragma once

#include "AstTypes.h"
#include <filesystem>

namespace ultra::ast {

class AstExtractor {
 public:
  /** Deprecated compatibility shim. Semantic extraction is handled by
   *  ultra::ai::SemanticExtractor + Tree-sitter pipeline. */
  static FileStructure extract(const std::filesystem::path& file);

  /** Always false in the unified Tree-sitter architecture. */
  static bool isLibClangAvailable();
};

}  // namespace ultra::ast
