#pragma once

#include "FileRegistry.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

enum class SymbolType : std::uint8_t {
  Unknown = 0,
  Class = 1,
  Function = 2,
  EntryPoint = 3,
  Import = 4,
  Export = 5,
  ReactComponent = 6
};

enum class Visibility : std::uint8_t {
  Unknown = 0,
  Public = 1,
  Private = 2,
  Protected = 3,
  Module = 4
};

struct ExtractedSymbol {
  std::string name;
  std::string signature;
  SymbolType symbolType{SymbolType::Unknown};
  Visibility visibility{Visibility::Unknown};
  std::uint32_t lineNumber{0};
};

struct SemanticSymbolDependency {
  std::string fromSymbol;
  std::string toSymbol;
  std::uint32_t lineNumber{0};
};

struct SemanticParseResult {
  std::vector<ExtractedSymbol> symbols;
  std::vector<std::string> dependencyReferences;
  std::vector<SemanticSymbolDependency> symbolDependencies;
  bool parsed{false};
  bool hasEntryPoint{false};
  std::string parseError;
};

class SemanticExtractor {
 public:
  static bool extract(const std::filesystem::path& path,
                      Language language,
                      SemanticParseResult& result,
                      std::string& error);
};

}  // namespace ultra::ai
