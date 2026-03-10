#pragma once

#include "AstTypes.h"
#include <filesystem>

namespace ultra::ast {

class AstExtractor {
 public:
  /** Extract structure from a single file. Uses libclang when available,
   *  otherwise falls back to regex. */
  static FileStructure extract(const std::filesystem::path& file);

  /** Returns true if libclang-backed extraction is available. */
  static bool isLibClangAvailable();
};

}  // namespace ultra::ast
