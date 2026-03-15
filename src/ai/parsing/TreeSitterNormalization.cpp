#include "TreeSitterNormalization.h"

#include <set>
#include <tuple>
//E:\Projects\Ultra\src\ai\parsing\TreeSitterNormalization.cpp
namespace ultra::ai::parsing {

namespace {

AstNodeKind mapKind(const SemanticSymbol& symbol) {
  switch (symbol.kind) {
    case SymbolKind::Function:
      return AstNodeKind::FunctionDecl;
    case SymbolKind::Method:
      return AstNodeKind::MethodDecl;
    case SymbolKind::Class:
      return symbol.isStruct ? AstNodeKind::StructDecl : AstNodeKind::ClassDecl;
    case SymbolKind::Variable:
      return AstNodeKind::VariableDecl;
    case SymbolKind::Import:
      return symbol.isInclude ? AstNodeKind::IncludeDirective
                              : AstNodeKind::ImportStatement;
    case SymbolKind::Call:
      return AstNodeKind::CallExpr;
    case SymbolKind::Namespace:
      return AstNodeKind::NamespaceDecl;
    case SymbolKind::Inheritance:
      return AstNodeKind::InheritanceRef;
    case SymbolKind::TypeRef:
      return AstNodeKind::TypeRef;
    default:
      return AstNodeKind::Root;
  }
}

}  // namespace

void TreeSitterNormalization::normalize(const std::vector<SemanticSymbol>& symbols,
                                        AstNode& root) {
  root.children.clear();

  std::set<std::tuple<std::uint8_t, std::string, std::string, std::uint32_t,
                      std::string>>
      dedupe;

  for (const SemanticSymbol& symbol : symbols) {
    const AstNodeKind kind = mapKind(symbol);
    if (kind == AstNodeKind::Root) {
      continue;
    }

    AstNode node;
    node.kind = kind;
    node.name = symbol.name;
    node.owner = symbol.owner;
    node.signature = symbol.signature;
    node.value = symbol.value;
    node.startLine = symbol.startLine;
    node.endLine = symbol.endLine;

    if ((node.kind == AstNodeKind::ImportStatement ||
         node.kind == AstNodeKind::IncludeDirective)) {
      if (node.value.empty()) {
        node.value = node.name;
      }
      if (node.name.empty()) {
        node.name = node.value;
      }
    }

    if (node.name.empty() && node.value.empty() &&
        node.kind != AstNodeKind::CallExpr &&
        node.kind != AstNodeKind::TypeRef &&
        node.kind != AstNodeKind::InheritanceRef) {
      continue;
    }

    const auto key = std::make_tuple(static_cast<std::uint8_t>(node.kind),
                                     node.name, node.owner, node.startLine,
                                     node.value);
    if (!dedupe.insert(key).second) {
      continue;
    }

    root.children.push_back(std::move(node));
  }
}

}  // namespace ultra::ai::parsing
