#include "ASTNormalizer.h"

#include <algorithm>
#include <cstdint>
#include <string>
//E:\Projects\Ultra\src\ai\parsing\ASTNormalizer.cpp
namespace ultra::ai::parsing::ASTNormalizer {

namespace {

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

bool isIdentifierType(const std::string& type) {
  if (type == "identifier" || type == "type_identifier") {
    return true;
  }
  return type.find("identifier") != std::string::npos;
}

std::string extractIdentifierName(TSNode node, const std::string& source) {
  const uint32_t childCount = ts_node_child_count(node);
  for (uint32_t i = 0; i < childCount; ++i) {
    TSNode child = ts_node_child(node, i);
    const char* childType = ts_node_type(child);
    if (childType == nullptr) {
      continue;
    }
    const std::string type(childType);
    if (!isIdentifierType(type)) {
      continue;
    }
    return sliceSource(source, child);
  }
  return {};
}

AstNodeKind mapKind(const std::string& type) {
  if (type == "function_definition" || type == "function_declaration") {
    return AstNodeKind::FunctionDecl;
  }
  if (type == "method_definition") {
    return AstNodeKind::MethodDecl;
  }
  if (type == "class_declaration" || type == "class_definition") {
    return AstNodeKind::ClassDecl;
  }
  if (type == "struct_specifier") {
    return AstNodeKind::StructDecl;
  }
  if (type == "call_expression") {
    return AstNodeKind::CallExpr;
  }
  if (type == "import_statement" || type == "import_declaration") {
    return AstNodeKind::ImportStatement;
  }
  if (type == "include_directive") {
    return AstNodeKind::IncludeDirective;
  }
  return AstNodeKind::Root;
}

}  // namespace

void walkTree(TSNode node, const std::string& source, AstNode& root) {
  const char* typeName = ts_node_type(node);
  if (typeName != nullptr) {
    const std::string type(typeName);
    const AstNodeKind kind = mapKind(type);
    if (kind != AstNodeKind::Root) {
      AstNode astNode;
      astNode.kind = kind;
      astNode.name = extractIdentifierName(node, source);
      astNode.startLine =
          static_cast<std::uint32_t>(ts_node_start_point(node).row);
      astNode.endLine =
          static_cast<std::uint32_t>(ts_node_end_point(node).row);
      root.children.push_back(std::move(astNode));
    }
  }

  const uint32_t childCount = ts_node_child_count(node);
  for (uint32_t i = 0; i < childCount; ++i) {
    TSNode child = ts_node_child(node, i);
    walkTree(child, source, root);
  }
}

}  // namespace ultra::ai::parsing::ASTNormalizer
