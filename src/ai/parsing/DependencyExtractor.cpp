#include "DependencyExtractor.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace ultra::ai::parsing {

namespace {

void gatherFileDependencies(const AstNode& node,
                            std::vector<std::string>& dependencies,
                            std::set<std::string>& dedupe) {
  if ((node.kind == AstNodeKind::ImportStatement ||
       node.kind == AstNodeKind::IncludeDirective) &&
      !node.value.empty()) {
    if (dedupe.insert(node.value).second) {
      dependencies.push_back(node.value);
    }
  }

  for (const AstNode& child : node.children) {
    gatherFileDependencies(child, dependencies, dedupe);
  }
}

void gatherSymbolDependencies(
    const AstNode& node,
    std::vector<SemanticSymbolDependency>& dependencies,
    std::set<std::tuple<std::string, std::string, std::uint32_t>>& dedupe) {
  const bool isDependencyNode =
      node.kind == AstNodeKind::CallExpr || node.kind == AstNodeKind::TypeRef ||
      node.kind == AstNodeKind::InheritanceRef;
  if (isDependencyNode && !node.owner.empty() && !node.name.empty()) {
    const auto key = std::make_tuple(node.owner, node.name, node.startLine);
    if (dedupe.insert(key).second) {
      SemanticSymbolDependency edge;
      edge.fromSymbol = node.owner;
      edge.toSymbol = node.name;
      edge.lineNumber = node.startLine;
      dependencies.push_back(std::move(edge));
    }
  }

  for (const AstNode& child : node.children) {
    gatherSymbolDependencies(child, dependencies, dedupe);
  }
}

}  // namespace

std::vector<std::string> DependencyExtractor::extractFileDependencies(
    const ParsedAST& ast) {
  std::vector<std::string> dependencies;
  if (!ast.valid) {
    return dependencies;
  }
  std::set<std::string> dedupe;
  gatherFileDependencies(ast.root, dependencies, dedupe);
  std::sort(dependencies.begin(), dependencies.end());
  return dependencies;
}

std::vector<SemanticSymbolDependency> DependencyExtractor::extractSymbolDependencies(
    const ParsedAST& ast) {
  std::vector<SemanticSymbolDependency> dependencies;
  if (!ast.valid) {
    return dependencies;
  }

  std::set<std::tuple<std::string, std::string, std::uint32_t>> dedupe;
  gatherSymbolDependencies(ast.root, dependencies, dedupe);
  std::sort(
      dependencies.begin(), dependencies.end(),
      [](const SemanticSymbolDependency& left,
         const SemanticSymbolDependency& right) {
        if (left.fromSymbol != right.fromSymbol) {
          return left.fromSymbol < right.fromSymbol;
        }
        if (left.toSymbol != right.toSymbol) {
          return left.toSymbol < right.toSymbol;
        }
        return left.lineNumber < right.lineNumber;
      });
  return dependencies;
}

}  // namespace ultra::ai::parsing

