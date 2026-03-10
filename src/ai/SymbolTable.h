#pragma once

#include "SemanticExtractor.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ultra::ai {

struct SymbolRecord {
  std::uint64_t symbolId{0};
  std::uint32_t fileId{0};
  std::string name;
  std::string signature;
  SymbolType symbolType{SymbolType::Unknown};
  Visibility visibility{Visibility::Unknown};
  std::uint32_t lineNumber{0};

  bool operator==(const SymbolRecord& other) const noexcept {
    return symbolId == other.symbolId && fileId == other.fileId &&
           name == other.name && signature == other.signature &&
           symbolType == other.symbolType &&
           visibility == other.visibility && lineNumber == other.lineNumber;
  }
  bool operator!=(const SymbolRecord& other) const noexcept {
    return !(*this == other);
  }
};

class SymbolTable {
 public:
  static constexpr std::uint32_t kLocalSymbolLimit = (1U << 20U) - 1U;

  static bool buildFromExtracted(std::uint32_t fileId,
                                 const std::vector<ExtractedSymbol>& extracted,
                                 std::vector<SymbolRecord>& outSymbols,
                                 std::string& error);
  static bool remapExisting(std::uint32_t fileId,
                            const std::vector<SymbolRecord>& existingSymbols,
                            std::vector<SymbolRecord>& outSymbols,
                            std::string& error);

  static std::uint64_t composeSymbolId(std::uint32_t fileId,
                                       std::uint32_t localSymbolIndex);
  static std::uint32_t extractFileId(std::uint64_t symbolId);
  static std::uint32_t extractLocalIndex(std::uint64_t symbolId);

  static void sortDeterministic(std::vector<SymbolRecord>& symbols);
  static std::map<std::uint32_t, std::vector<SymbolRecord>> groupByFileId(
      const std::vector<SymbolRecord>& symbols);
};

}  // namespace ultra::ai

