#pragma once

#include "TreeSitterParser.h"
//E:\Projects\Ultra\src\ai\parsing\ASTNormalizer.h
#include <string>
#include <tree_sitter/api.h>

namespace ultra::ai::parsing::ASTNormalizer {

void walkTree(TSNode node, const std::string& source, AstNode& root);

}  // namespace ultra::ai::parsing::ASTNormalizer
