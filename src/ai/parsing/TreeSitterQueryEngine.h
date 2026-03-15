#pragma once

#include "../FileRegistry.h"

#include <cstdint>
#include <string>
#include <vector>
//E:\Projects\Ultra\src\ai\parsing\TreeSitterQueryEngine.h
#if __has_include(<tree_sitter/api.h>)
#include <tree_sitter/api.h>
#ifndef ULTRA_HAS_TREE_SITTER
#define ULTRA_HAS_TREE_SITTER 1
#endif
#else
#ifndef ULTRA_HAS_TREE_SITTER
#define ULTRA_HAS_TREE_SITTER 0
#endif
struct TSNode;
#endif

namespace ultra::ai::parsing {

enum class SymbolKind : std::uint8_t {
  Unknown = 0,
  Function = 1,
  Class = 2,
  Method = 3,
  Variable = 4,
  Import = 5,
  Call = 6,
  Namespace = 7,
  Inheritance = 8,
  TypeRef = 9
};

struct SemanticSymbol {
  SymbolKind kind{SymbolKind::Unknown};
  std::string name;
  std::string owner;
  std::string signature;
  std::string value;
  std::uint32_t startLine{0};
  std::uint32_t endLine{0};
  bool isStruct{false};
  bool isInclude{false};
};

class TreeSitterQueryEngine {
 public:
  static std::vector<SemanticSymbol> execute(const TSNode& root,
                                             const std::string& source,
                                             Language language);
};

}  // namespace ultra::ai::parsing
