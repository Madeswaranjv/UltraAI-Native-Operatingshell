#pragma once

#include "../FileRegistry.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
//E:\Projects\Ultra\src\ai\parsing\TreeSitterParser.h
namespace ultra::ai::parsing {

enum class AstNodeKind : std::uint8_t {
  Root = 0,
  NamespaceDecl = 1,
  ClassDecl = 2,
  StructDecl = 3,
  FunctionDecl = 4,
  MethodDecl = 5,
  VariableDecl = 6,
  IncludeDirective = 7,
  ImportStatement = 8,
  ExportStatement = 9,
  InheritanceRef = 10,
  TypeRef = 11,
  CallExpr = 12,
  ReactComponentDecl = 13
};

struct AstNode {
  AstNodeKind kind{AstNodeKind::Root};
  std::string name;
  std::string value;
  std::string signature;
  std::string owner;
  std::uint32_t startLine{0};
  std::uint32_t endLine{0};
  std::vector<AstNode> children;
};

struct ParsedAST {
  Language language{Language::Unknown};
  std::filesystem::path sourcePath;
  AstNode root{};
  bool valid{false};
  bool usedTreeSitterBackend{false};
};

class TreeSitterParser {
 public:
  bool parseFile(const std::filesystem::path& path,
                 Language language,
                 ParsedAST& ast) const;
};

}  // namespace ultra::ai::parsing

