#pragma once

#include "TreeSitterParser.h"
#include "TreeSitterQueryEngine.h"
//E:\Projects\Ultra\src\ai\parsing\TreeSitterNormalization.h
#include <vector>

namespace ultra::ai::parsing {

class TreeSitterNormalization {
 public:
  static void normalize(const std::vector<SemanticSymbol>& symbols, AstNode& root);
};

}  // namespace ultra::ai::parsing
