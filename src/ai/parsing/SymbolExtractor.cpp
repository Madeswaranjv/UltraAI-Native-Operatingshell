#include "SymbolExtractor.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace ultra::ai::parsing {

namespace {

Visibility visibilityForSymbol(const AstNode& node) {
  if (node.kind == AstNodeKind::ImportStatement ||
      node.kind == AstNodeKind::ExportStatement) {
    return Visibility::Module;
  }
  if (!node.name.empty() && node.name[0] == '_') {
    return Visibility::Private;
  }
  return Visibility::Public;
}

void maybeAppendSymbol(const AstNode& node,
                       std::vector<ExtractedSymbol>& symbols,
                       std::set<std::tuple<std::uint32_t, std::string, std::uint8_t,
                                           std::uint8_t>>& dedupe) {
  ExtractedSymbol symbol;
  bool include = true;

  switch (node.kind) {
    case AstNodeKind::ClassDecl:
    case AstNodeKind::StructDecl:
      symbol.symbolType = SymbolType::Class;
      symbol.name = node.name;
      break;

    case AstNodeKind::FunctionDecl:
    case AstNodeKind::MethodDecl:
      symbol.symbolType =
          node.name == "main" ? SymbolType::EntryPoint : SymbolType::Function;
      symbol.name = node.name;
      break;

    case AstNodeKind::ReactComponentDecl:
      symbol.symbolType = SymbolType::ReactComponent;
      symbol.name = node.name;
      break;

    case AstNodeKind::ImportStatement:
    case AstNodeKind::IncludeDirective:
      symbol.symbolType = SymbolType::Import;
      symbol.name = node.value.empty() ? node.name : node.value;
      break;

    case AstNodeKind::ExportStatement:
      symbol.symbolType = SymbolType::Export;
      symbol.name = node.name;
      break;

    case AstNodeKind::NamespaceDecl:
    case AstNodeKind::VariableDecl:
      symbol.symbolType = SymbolType::Unknown;
      symbol.name = node.name;
      break;

    default:
      include = false;
      break;
  }

  if (!include || symbol.name.empty()) {
    return;
  }

  symbol.signature = node.signature;
  symbol.visibility = visibilityForSymbol(node);
  symbol.lineNumber = node.startLine;

  const auto dedupeKey = std::make_tuple(
      symbol.lineNumber, symbol.name, static_cast<std::uint8_t>(symbol.symbolType),
      static_cast<std::uint8_t>(symbol.visibility));
  if (!dedupe.insert(dedupeKey).second) {
    return;
  }

  symbols.push_back(std::move(symbol));
}

void traverse(const AstNode& node,
              std::vector<ExtractedSymbol>& symbols,
              std::set<std::tuple<std::uint32_t, std::string, std::uint8_t,
                                  std::uint8_t>>& dedupe) {
  maybeAppendSymbol(node, symbols, dedupe);
  for (const AstNode& child : node.children) {
    traverse(child, symbols, dedupe);
  }
}

}  // namespace

std::vector<ExtractedSymbol> SymbolExtractor::extract(const ParsedAST& ast) {
  std::vector<ExtractedSymbol> symbols;
  if (!ast.valid) {
    return symbols;
  }

  std::set<std::tuple<std::uint32_t, std::string, std::uint8_t, std::uint8_t>>
      dedupe;
  traverse(ast.root, symbols, dedupe);

  std::sort(symbols.begin(), symbols.end(),
            [](const ExtractedSymbol& left, const ExtractedSymbol& right) {
              if (left.lineNumber != right.lineNumber) {
                return left.lineNumber < right.lineNumber;
              }
              if (left.name != right.name) {
                return left.name < right.name;
              }
              if (left.symbolType != right.symbolType) {
                return static_cast<std::uint8_t>(left.symbolType) <
                       static_cast<std::uint8_t>(right.symbolType);
              }
              return static_cast<std::uint8_t>(left.visibility) <
                     static_cast<std::uint8_t>(right.visibility);
            });
  return symbols;
}

}  // namespace ultra::ai::parsing

