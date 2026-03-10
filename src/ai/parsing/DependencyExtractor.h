#pragma once

#include "../SemanticExtractor.h"
#include "TreeSitterParser.h"

#include <vector>

namespace ultra::ai::parsing {

class DependencyExtractor {
 public:
  static std::vector<std::string> extractFileDependencies(const ParsedAST& ast);
  static std::vector<SemanticSymbolDependency> extractSymbolDependencies(
      const ParsedAST& ast);
};

}  // namespace ultra::ai::parsing

