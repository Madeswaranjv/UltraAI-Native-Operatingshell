#include "TreeSitterLanguages.h"

extern "C" {
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_javascript();
const TSLanguage* tree_sitter_typescript();
const TSLanguage* tree_sitter_python();
const TSLanguage* tree_sitter_java();
const TSLanguage* tree_sitter_go();
const TSLanguage* tree_sitter_rust();
const TSLanguage* tree_sitter_c_sharp();
}

namespace ultra::ai::parsing::TreeSitterLanguages {

const TSLanguage* getTreeSitterLanguage(const Language lang) {
  switch (lang) {
    case Language::Cpp:
      return tree_sitter_cpp();
    case Language::JavaScript:
      return tree_sitter_javascript();
    case Language::TypeScript:
      return tree_sitter_typescript();
    case Language::Python:
      return tree_sitter_python();
    case Language::Java:
      return tree_sitter_java();
    case Language::Go:
      return tree_sitter_go();
    case Language::Rust:
      return tree_sitter_rust();
    case Language::CSharp:
      return tree_sitter_c_sharp();
    default:
      return nullptr;
  }
}

}  // namespace ultra::ai::parsing::TreeSitterLanguages
