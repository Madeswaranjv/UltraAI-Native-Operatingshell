#include "SemanticExtractor.h"

#include "parsing/DependencyExtractor.h"
#include "parsing/SymbolExtractor.h"
#include "parsing/TreeSitterParser.h"

#include <algorithm>

namespace ultra::ai {

SemanticParseResult SemanticExtractor::extract(const std::filesystem::path& path,
                                               const Language language) {
  SemanticParseResult result;

  parsing::ParsedAST ast;
  parsing::TreeSitterParser parser;
  if (!parser.parseFile(path, language, ast)) {
    return result;
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

  return result;
}

}  // namespace ultra::ai
