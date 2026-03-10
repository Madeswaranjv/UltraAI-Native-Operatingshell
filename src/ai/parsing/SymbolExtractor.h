#pragma once

#include "../SemanticExtractor.h"
#include "TreeSitterParser.h"

#include <vector>

namespace ultra::ai::parsing {

class SymbolExtractor {
 public:
  static std::vector<ExtractedSymbol> extract(const ParsedAST& ast);
};

}  // namespace ultra::ai::parsing

