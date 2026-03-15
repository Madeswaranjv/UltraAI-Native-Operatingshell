#include "SemanticExtractor.h"

#include "parsing/DependencyExtractor.h"
#include "parsing/SymbolExtractor.h"
#include "parsing/TreeSitterParser.h"

#include <algorithm>

namespace ultra::ai {

bool SemanticExtractor::extract(const std::filesystem::path& path,
                                const Language language,
                                SemanticParseResult& result,
                                std::string& error) {
  result = SemanticParseResult{};
  error.clear();

  parsing::ParsedAST ast;
  parsing::TreeSitterParser parser;
  if (!parser.parseFile(path, language, ast)) {
    result.parseError =
        "Tree-sitter parse failed for: " + path.lexically_normal().string();
    error = result.parseError;
    return false;
  }
  if (!ast.valid || !ast.usedTreeSitterBackend) {
    result.parseError =
        "Tree-sitter AST is unavailable for: " + path.lexically_normal().string();
    error = result.parseError;
    return false;
  }

  result.symbols = parsing::SymbolExtractor::extract(ast);
  result.dependencyReferences =
      parsing::DependencyExtractor::extractFileDependencies(ast);
  result.symbolDependencies =
      parsing::DependencyExtractor::extractSymbolDependencies(ast);

  result.hasEntryPoint =
      std::any_of(result.symbols.begin(), result.symbols.end(),
                  [](const ExtractedSymbol& symbol) {
                    return symbol.symbolType == SymbolType::EntryPoint ||
                           symbol.name == "main";
                  });
  result.parsed = true;

  return true;
}

}  // namespace ultra::ai
