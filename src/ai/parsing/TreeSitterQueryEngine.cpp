#include "TreeSitterQueryEngine.h"

#include "TreeSitterLanguages.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
//E:\Projects\Ultra\src\ai\parsing\TreeSitterQueryEngine.cpp
namespace ultra::ai::parsing {

#if ULTRA_HAS_TREE_SITTER

namespace {

struct QueryPattern {
  SymbolKind kind;
  const char* capture;
  const char* query;
  bool isStruct;
  bool isInclude;
};

const std::vector<QueryPattern>& patternsForLanguage(const Language language) {
  static const std::vector<QueryPattern> kCpp{
      {SymbolKind::Function, "function", "(function_definition) @function", false, false},
      {SymbolKind::Function, "function", "(function_declaration) @function", false, false},
      {SymbolKind::Class, "class", "(class_specifier) @class", false, false},
      {SymbolKind::Class, "struct", "(struct_specifier) @struct", true, false},
      {SymbolKind::Namespace, "namespace", "(namespace_definition) @namespace", false, false},
      {SymbolKind::Import, "include", "(preproc_include) @include", false, true},
      {SymbolKind::Call, "call", "(call_expression) @call", false, false},
      {SymbolKind::Variable, "variable", "(init_declarator) @variable", false, false},
      {SymbolKind::Variable, "variable", "(field_declaration) @variable", false, false},
      {SymbolKind::TypeRef, "typeref", "(type_identifier) @typeref", false, false},
      {SymbolKind::TypeRef, "typeref", "(qualified_identifier) @typeref", false, false}};
  static const std::vector<QueryPattern> kJs{
      {SymbolKind::Function, "function", "(function_declaration) @function", false, false},
      {SymbolKind::Method, "method", "(method_definition) @method", false, false},
      {SymbolKind::Class, "class", "(class_declaration) @class", false, false},
      {SymbolKind::Import, "import", "(import_statement) @import", false, false},
      {SymbolKind::Call, "call", "(call_expression) @call", false, false},
      {SymbolKind::Variable, "variable", "(variable_declarator) @variable", false, false}};
  static const std::vector<QueryPattern> kTs{
      {SymbolKind::Function, "function", "(function_declaration) @function", false, false},
      {SymbolKind::Method, "method", "(method_definition) @method", false, false},
      {SymbolKind::Class, "class", "(class_declaration) @class", false, false},
      {SymbolKind::Import, "import", "(import_statement) @import", false, false},
      {SymbolKind::Call, "call", "(call_expression) @call", false, false},
      {SymbolKind::Variable, "variable", "(variable_declarator) @variable", false, false},
      {SymbolKind::Namespace, "namespace", "(namespace_declaration) @namespace", false, false},
      {SymbolKind::TypeRef, "typeref", "(type_identifier) @typeref", false, false},
      {SymbolKind::TypeRef, "typeref", "(type_reference) @typeref", false, false}};
  static const std::vector<QueryPattern> kPython{
      {SymbolKind::Function, "function", "(function_definition) @function", false, false},
      {SymbolKind::Class, "class", "(class_definition) @class", false, false},
      {SymbolKind::Import, "import", "(import_statement) @import", false, false},
      {SymbolKind::Import, "import", "(import_from_statement) @import", false, false},
      {SymbolKind::Call, "call", "(call) @call", false, false},
      {SymbolKind::Variable, "variable", "(assignment) @variable", false, false}};

  switch (language) {
    case Language::Cpp:
      return kCpp;
    case Language::JavaScript:
      return kJs;
    case Language::TypeScript:
      return kTs;
    case Language::Python:
      return kPython;
    default: {
      static const std::vector<QueryPattern> kEmpty{};
      return kEmpty;
    }
  }
}

std::string trimCopy(std::string value) {
  std::size_t start = 0U;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }
  if (start >= value.size()) {
    return {};
  }
  std::size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(start, end - start);
}

std::string collapseWhitespace(std::string value) {
  std::string out;
  out.reserve(value.size());
  bool inSpace = false;
  for (const char ch : value) {
    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
      continue;
    }
    inSpace = false;
    out.push_back(ch);
  }
  return trimCopy(out);
}

std::string sliceSource(const std::string& source, TSNode node) {
  const uint32_t start = ts_node_start_byte(node);
  const uint32_t end = ts_node_end_byte(node);
  if (start >= source.size() || end <= start) {
    return {};
  }
  const std::size_t safeEnd = std::min<std::size_t>(end, source.size());
  if (safeEnd <= start) {
    return {};
  }
  return source.substr(start, safeEnd - start);
}

bool isIdentifierLikeType(std::string_view type) {
  if (type.find("identifier") != std::string_view::npos) {
    return true;
  }
  return type == "operator_name" || type == "destructor_name";
}

std::string extractIdentifierFromNode(TSNode node, const std::string& source) {
  std::vector<TSNode> stack;
  stack.push_back(node);
  while (!stack.empty()) {
    TSNode current = stack.back();
    stack.pop_back();
    const char* typeName = ts_node_type(current);
    if (typeName != nullptr) {
      std::string_view type(typeName);
      if (isIdentifierLikeType(type)) {
        return trimCopy(sliceSource(source, current));
      }
    }
    const uint32_t childCount = ts_node_child_count(current);
    for (uint32_t i = 0; i < childCount; ++i) {
      stack.push_back(ts_node_child(current, i));
    }
  }
  return {};
}

std::string extractLastIdentifierFromNode(TSNode node, const std::string& source) {
  std::string last;
  std::vector<TSNode> stack;
  stack.push_back(node);
  while (!stack.empty()) {
    TSNode current = stack.back();
    stack.pop_back();
    const char* typeName = ts_node_type(current);
    if (typeName != nullptr) {
      std::string_view type(typeName);
      if (isIdentifierLikeType(type)) {
        last = trimCopy(sliceSource(source, current));
      }
    }
    const uint32_t childCount = ts_node_child_count(current);
    for (uint32_t i = 0; i < childCount; ++i) {
      stack.push_back(ts_node_child(current, i));
    }
  }
  return last;
}

TSNode childByField(TSNode node, const char* field) {
  return ts_node_child_by_field_name(
      node, field, static_cast<uint32_t>(std::strlen(field)));
}

std::string extractNameByField(TSNode node,
                               const std::string& source,
                               const char* field) {
  TSNode nameNode = childByField(node, field);
  if (ts_node_is_null(nameNode)) {
    return {};
  }
  return trimCopy(sliceSource(source, nameNode));
}

std::string extractFunctionName(TSNode node, const std::string& source) {
  std::string name = extractNameByField(node, source, "name");
  if (!name.empty()) {
    return name;
  }
  TSNode declarator = childByField(node, "declarator");
  if (!ts_node_is_null(declarator)) {
    name = extractIdentifierFromNode(declarator, source);
    if (!name.empty()) {
      return name;
    }
  }
  TSNode functionNode = childByField(node, "function");
  if (!ts_node_is_null(functionNode)) {
    name = extractLastIdentifierFromNode(functionNode, source);
    if (!name.empty()) {
      return name;
    }
  }
  return extractIdentifierFromNode(node, source);
}

std::string extractClassName(TSNode node, const std::string& source) {
  std::string name = extractNameByField(node, source, "name");
  if (!name.empty()) {
    return name;
  }
  return extractIdentifierFromNode(node, source);
}

std::string extractVariableName(TSNode node, const std::string& source) {
  std::string name = extractNameByField(node, source, "name");
  if (!name.empty()) {
    return name;
  }
  TSNode declarator = childByField(node, "declarator");
  if (!ts_node_is_null(declarator)) {
    name = extractIdentifierFromNode(declarator, source);
    if (!name.empty()) {
      return name;
    }
  }
  return extractIdentifierFromNode(node, source);
}

std::string extractCallName(TSNode node, const std::string& source) {
  TSNode functionNode = childByField(node, "function");
  if (!ts_node_is_null(functionNode)) {
    std::string name = extractLastIdentifierFromNode(functionNode, source);
    if (!name.empty()) {
      return name;
    }
  }
  std::string name = extractNameByField(node, source, "name");
  if (!name.empty()) {
    return name;
  }
  return extractLastIdentifierFromNode(node, source);
}

std::string extractQuotedString(const std::string& text) {
  const std::size_t firstQuote = text.find_first_of("'\"");
  if (firstQuote == std::string::npos) {
    return {};
  }
  const char quote = text[firstQuote];
  const std::size_t secondQuote = text.find(quote, firstQuote + 1U);
  if (secondQuote == std::string::npos || secondQuote <= firstQuote + 1U) {
    return {};
  }
  return text.substr(firstQuote + 1U, secondQuote - firstQuote - 1U);
}

std::string extractImportValue(const std::string& text, bool isInclude) {
  if (isInclude) {
    const std::size_t firstAngle = text.find('<');
    if (firstAngle != std::string::npos) {
      const std::size_t lastAngle = text.find('>', firstAngle + 1U);
      if (lastAngle != std::string::npos && lastAngle > firstAngle + 1U) {
        return text.substr(firstAngle + 1U, lastAngle - firstAngle - 1U);
      }
    }
  }
  return extractQuotedString(text);
}

std::string stripQualifiers(std::string symbol) {
  while (!symbol.empty() && (symbol.back() == ':' || symbol.back() == '.')) {
    symbol.pop_back();
  }
  const std::size_t nsPos = symbol.rfind("::");
  if (nsPos != std::string::npos) {
    symbol = symbol.substr(nsPos + 2U);
  }
  const std::size_t dotPos = symbol.rfind('.');
  if (dotPos != std::string::npos) {
    symbol = symbol.substr(dotPos + 1U);
  }
  return symbol;
}

std::vector<std::string> splitCsv(const std::string& value) {
  std::vector<std::string> out;
  std::string current;
  int parenDepth = 0;
  int angleDepth = 0;
  for (const char ch : value) {
    if (ch == '(') {
      ++parenDepth;
    } else if (ch == ')' && parenDepth > 0) {
      --parenDepth;
    } else if (ch == '<') {
      ++angleDepth;
    } else if (ch == '>' && angleDepth > 0) {
      --angleDepth;
    }
    if (ch == ',' && parenDepth == 0 && angleDepth == 0) {
      out.push_back(trimCopy(current));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!trimCopy(current).empty()) {
    out.push_back(trimCopy(current));
  }
  return out;
}

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::vector<std::string> parseInheritanceList(const std::string& signature,
                                               const Language language) {
  std::vector<std::string> bases;
  if (signature.empty()) {
    return bases;
  }

  if (language == Language::Cpp) {
    const std::size_t colonPos = signature.find(':');
    if (colonPos == std::string::npos) {
      return bases;
    }
    std::string tail = signature.substr(colonPos + 1U);
    const std::size_t bracePos = tail.find('{');
    if (bracePos != std::string::npos) {
      tail = tail.substr(0U, bracePos);
    }
    const std::vector<std::string> parts = splitCsv(tail);
    for (std::string base : parts) {
      base = trimCopy(base);
      if (base.empty()) {
        continue;
      }
      const std::string lower = toLower(base);
      if (lower.rfind("public", 0U) == 0U) {
        base = trimCopy(base.substr(6U));
      } else if (lower.rfind("protected", 0U) == 0U) {
        base = trimCopy(base.substr(9U));
      } else if (lower.rfind("private", 0U) == 0U) {
        base = trimCopy(base.substr(7U));
      }
      if (toLower(base).rfind("virtual", 0U) == 0U) {
        base = trimCopy(base.substr(7U));
      }
      base = stripQualifiers(base);
      if (!base.empty()) {
        bases.push_back(base);
      }
    }
    return bases;
  }

  if (language == Language::JavaScript || language == Language::TypeScript) {
    const std::string lower = toLower(signature);
    const std::string marker = "extends ";
    const std::size_t extendsPos = lower.find(marker);
    if (extendsPos == std::string::npos) {
      return bases;
    }
    std::string tail = signature.substr(extendsPos + marker.size());
    const std::size_t bracePos = tail.find('{');
    if (bracePos != std::string::npos) {
      tail = tail.substr(0U, bracePos);
    }
    std::string base = trimCopy(tail);
    const std::string lowerBase = toLower(base);
    const std::size_t implementsPos = lowerBase.find("implements ");
    if (implementsPos != std::string::npos) {
      base = trimCopy(base.substr(0U, implementsPos));
    }
    const std::size_t spacePos = base.find_first_of(" \t");
    if (spacePos != std::string::npos) {
      base = trimCopy(base.substr(0U, spacePos));
    }
    base = stripQualifiers(base);
    if (!base.empty()) {
      bases.push_back(base);
    }
    return bases;
  }

  if (language == Language::Python) {
    const std::size_t openParen = signature.find('(');
    const std::size_t closeParen = signature.find(')', openParen + 1U);
    if (openParen != std::string::npos && closeParen != std::string::npos &&
        closeParen > openParen + 1U) {
      const std::string inside =
          signature.substr(openParen + 1U, closeParen - openParen - 1U);
      const std::vector<std::string> parts = splitCsv(inside);
      for (std::string base : parts) {
        base = stripQualifiers(trimCopy(base));
        if (!base.empty()) {
          bases.push_back(base);
        }
      }
    }
  }

  return bases;
}

bool isFunctionLike(std::string_view type) {
  static const std::string_view kTypes[] = {
      "function_definition", "function_declaration", "function", "function_item",
      "function_signature", "function_expression", "method_definition",
      "method_declaration", "constructor_definition", "arrow_function",
      "lambda_expression"};
  for (const auto& candidate : kTypes) {
    if (type == candidate) {
      return true;
    }
  }
  return false;
}

bool isClassLike(std::string_view type) {
  static const std::string_view kTypes[] = {
      "class_specifier", "class_definition", "class_declaration",
      "struct_specifier", "struct_declaration", "interface_declaration"};
  for (const auto& candidate : kTypes) {
    if (type == candidate) {
      return true;
    }
  }
  return false;
}

struct OwnerInfo {
  std::string name;
  bool isClass{false};
  bool isFunction{false};
};

OwnerInfo findOwnerInfo(TSNode node, const std::string& source) {
  OwnerInfo info;
  TSNode current = node;
  while (true) {
    current = ts_node_parent(current);
    if (ts_node_is_null(current)) {
      break;
    }
    const char* typeName = ts_node_type(current);
    if (typeName == nullptr) {
      continue;
    }
    const std::string_view type(typeName);
    if (!info.isFunction && isFunctionLike(type)) {
      std::string name = extractFunctionName(current, source);
      if (!name.empty()) {
        info.name = name;
        info.isFunction = true;
        return info;
      }
    }
    if (!info.isClass && isClassLike(type)) {
      std::string name = extractClassName(current, source);
      if (!name.empty()) {
        info.name = name;
        info.isClass = true;
      }
    }
  }
  return info;
}

bool isDefinitionNameNode(TSNode node) {
  TSNode parent = ts_node_parent(node);
  if (ts_node_is_null(parent)) {
    return false;
  }
  const char* typeName = ts_node_type(parent);
  if (typeName == nullptr) {
    return false;
  }
  const std::string_view type(typeName);
  if (type != "class_specifier" && type != "struct_specifier" &&
      type != "class_definition" && type != "class_declaration" &&
      type != "interface_declaration" && type != "function_definition" &&
      type != "function_declaration" && type != "method_definition" &&
      type != "namespace_definition" && type != "namespace_declaration") {
    return false;
  }
  TSNode nameNode = childByField(parent, "name");
  if (ts_node_is_null(nameNode)) {
    return false;
  }
  return ts_node_eq(nameNode, node);
}

struct QueryCache {
  std::unordered_map<std::string, TSQuery*> queries;
  std::unordered_map<std::string, bool> failed;

  ~QueryCache() {
    for (auto& entry : queries) {
      if (entry.second != nullptr) {
        ts_query_delete(entry.second);
      }
    }
  }
};

QueryCache& queryCacheFor(const Language language) {
  static thread_local std::unordered_map<int, QueryCache> caches;
  return caches[static_cast<int>(language)];
}

TSQuery* getQuery(const TSLanguage* language,
                  QueryCache& cache,
                  const QueryPattern& pattern) {
  const std::string key(pattern.query);
  auto existing = cache.queries.find(key);
  if (existing != cache.queries.end()) {
    return existing->second;
  }
  if (cache.failed.find(key) != cache.failed.end()) {
    return nullptr;
  }

  uint32_t errorOffset = 0;
  TSQueryError errorType = TSQueryErrorNone;
  TSQuery* query = ts_query_new(
      language, pattern.query,
      static_cast<uint32_t>(std::strlen(pattern.query)),
      &errorOffset, &errorType);
  if (query == nullptr) {
    cache.failed.emplace(key, true);
    return nullptr;
  }

  cache.queries.emplace(key, query);
  return query;
}

}  // namespace

std::vector<SemanticSymbol> TreeSitterQueryEngine::execute(
    const TSNode& root,
    const std::string& source,
    const Language language) {
  std::vector<SemanticSymbol> symbols;

  const TSLanguage* tsLanguage = TreeSitterLanguages::getTreeSitterLanguage(language);
  if (tsLanguage == nullptr) {
    return symbols;
  }

  const auto& patterns = patternsForLanguage(language);
  if (patterns.empty()) {
    return symbols;
  }

  QueryCache& cache = queryCacheFor(language);
  TSQueryCursor* cursor = ts_query_cursor_new();
  if (cursor == nullptr) {
    return symbols;
  }

  for (const QueryPattern& pattern : patterns) {
    TSQuery* query = getQuery(tsLanguage, cache, pattern);
    if (query == nullptr) {
      continue;
    }

    ts_query_cursor_exec(cursor, query, root);
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
      for (uint32_t i = 0; i < match.capture_count; ++i) {
        const TSNode node = match.captures[i].node;

        SemanticSymbol symbol;
        symbol.kind = pattern.kind;
        symbol.isStruct = pattern.isStruct;
        symbol.isInclude = pattern.isInclude;
        symbol.startLine = ts_node_start_point(node).row + 1U;
        symbol.endLine = ts_node_end_point(node).row + 1U;
        symbol.signature = collapseWhitespace(sliceSource(source, node));

        bool skipDefaultPush = false;

        switch (pattern.kind) {
          case SymbolKind::Function: {
            symbol.name = extractFunctionName(node, source);
            OwnerInfo owner = findOwnerInfo(node, source);
            if (owner.isClass && !owner.name.empty()) {
              symbol.kind = SymbolKind::Method;
              symbol.owner = owner.name;
            } else if (!owner.name.empty()) {
              symbol.owner = owner.name;
            }
            break;
          }
          case SymbolKind::Method: {
            symbol.name = extractFunctionName(node, source);
            OwnerInfo owner = findOwnerInfo(node, source);
            symbol.owner = owner.name;
            break;
          }
          case SymbolKind::Class: {
            symbol.name = extractClassName(node, source);
            OwnerInfo owner = findOwnerInfo(node, source);
            if (!owner.name.empty() && !owner.isFunction) {
              symbol.owner = owner.name;
            }
            if (!symbol.name.empty()) {
              symbols.push_back(symbol);
              const std::vector<std::string> bases =
                  parseInheritanceList(symbol.signature, language);
              for (const std::string& baseName : bases) {
                if (baseName.empty()) {
                  continue;
                }
                SemanticSymbol inherit;
                inherit.kind = SymbolKind::Inheritance;
                inherit.name = baseName;
                inherit.owner = symbol.name;
                inherit.signature = symbol.signature;
                inherit.startLine = symbol.startLine;
                inherit.endLine = symbol.endLine;
                symbols.push_back(std::move(inherit));
              }
              skipDefaultPush = true;
            }
            break;
          }
          case SymbolKind::Variable: {
            symbol.name = extractVariableName(node, source);
            OwnerInfo owner = findOwnerInfo(node, source);
            symbol.owner = owner.name;
            break;
          }
          case SymbolKind::Import: {
            symbol.value = extractImportValue(symbol.signature, symbol.isInclude);
            if (symbol.name.empty()) {
              symbol.name = symbol.value;
            }
            break;
          }
          case SymbolKind::Call: {
            symbol.name = extractCallName(node, source);
            OwnerInfo owner = findOwnerInfo(node, source);
            symbol.owner = owner.name;
            break;
          }
          case SymbolKind::Namespace: {
            symbol.name = extractNameByField(node, source, "name");
            if (symbol.name.empty()) {
              symbol.name = extractIdentifierFromNode(node, source);
            }
            break;
          }
          case SymbolKind::TypeRef: {
            if (isDefinitionNameNode(node)) {
              continue;
            }
            symbol.name = extractIdentifierFromNode(node, source);
            symbol.name = stripQualifiers(symbol.name);
            OwnerInfo owner = findOwnerInfo(node, source);
            symbol.owner = owner.name;
            break;
          }
          default:
            break;
        }

        if (skipDefaultPush) {
          continue;
        }

        if (symbol.kind != SymbolKind::Import &&
            symbol.kind != SymbolKind::Call &&
            symbol.kind != SymbolKind::TypeRef &&
            symbol.kind != SymbolKind::Inheritance &&
            symbol.name.empty()) {
          continue;
        }
        if (symbol.kind == SymbolKind::Call && symbol.name.empty()) {
          continue;
        }
        if (symbol.kind == SymbolKind::TypeRef && symbol.name.empty()) {
          continue;
        }

        symbols.push_back(std::move(symbol));
      }
    }
  }

  ts_query_cursor_delete(cursor);
  return symbols;
}

#else

std::vector<SemanticSymbol> TreeSitterQueryEngine::execute(
    const TSNode& /*root*/,
    const std::string& /*source*/,
    const Language /*language*/) {
  return {};
}

#endif

}  // namespace ultra::ai::parsing
