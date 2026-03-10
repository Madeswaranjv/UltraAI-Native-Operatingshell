#include "SymbolTable.h"

#include <algorithm>
#include <tuple>

namespace ultra::ai {

namespace {

bool symbolLess(const SymbolRecord& left, const SymbolRecord& right) {
  return std::tie(left.fileId, left.lineNumber, left.name, left.signature,
                  left.symbolType, left.visibility) <
         std::tie(right.fileId, right.lineNumber, right.name, right.signature,
                  right.symbolType, right.visibility);
}

}  // namespace

bool SymbolTable::buildFromExtracted(
    const std::uint32_t fileId,
    const std::vector<ExtractedSymbol>& extracted,
    std::vector<SymbolRecord>& outSymbols,
    std::string& error) {
  outSymbols.clear();
  outSymbols.reserve(extracted.size());

  for (const ExtractedSymbol& item : extracted) {
    SymbolRecord symbol;
    symbol.fileId = fileId;
    symbol.name = item.name;
    symbol.signature = item.signature;
    symbol.symbolType = item.symbolType;
    symbol.visibility = item.visibility;
    symbol.lineNumber = item.lineNumber;
    outSymbols.push_back(std::move(symbol));
  }

  std::sort(outSymbols.begin(), outSymbols.end(), symbolLess);
  for (std::size_t i = 0; i < outSymbols.size(); ++i) {
    const std::uint32_t localIndex = static_cast<std::uint32_t>(i + 1U);
    if (localIndex > kLocalSymbolLimit) {
      error = "Symbol limit exceeded for file_id=" + std::to_string(fileId);
      return false;
    }
    outSymbols[i].symbolId = composeSymbolId(fileId, localIndex);
  }
  return true;
}

bool SymbolTable::remapExisting(std::uint32_t fileId,
                                const std::vector<SymbolRecord>& existingSymbols,
                                std::vector<SymbolRecord>& outSymbols,
                                std::string& error) {
  outSymbols = existingSymbols;
  for (SymbolRecord& symbol : outSymbols) {
    symbol.fileId = fileId;
  }
  std::sort(outSymbols.begin(), outSymbols.end(), symbolLess);

  for (std::size_t i = 0; i < outSymbols.size(); ++i) {
    const std::uint32_t localIndex = static_cast<std::uint32_t>(i + 1U);
    if (localIndex > kLocalSymbolLimit) {
      error = "Symbol limit exceeded while remapping file_id=" +
              std::to_string(fileId);
      return false;
    }
    outSymbols[i].symbolId = composeSymbolId(fileId, localIndex);
  }
  return true;
}

std::uint64_t SymbolTable::composeSymbolId(const std::uint32_t fileId,
                                           const std::uint32_t localSymbolIndex) {
  return (static_cast<std::uint64_t>(fileId) << 20U) |
         static_cast<std::uint64_t>(localSymbolIndex);
}

std::uint32_t SymbolTable::extractFileId(const std::uint64_t symbolId) {
  return static_cast<std::uint32_t>(symbolId >> 20U);
}

std::uint32_t SymbolTable::extractLocalIndex(const std::uint64_t symbolId) {
  return static_cast<std::uint32_t>(symbolId & static_cast<std::uint64_t>(kLocalSymbolLimit));
}

void SymbolTable::sortDeterministic(std::vector<SymbolRecord>& symbols) {
  std::sort(symbols.begin(), symbols.end(), symbolLess);
}

std::map<std::uint32_t, std::vector<SymbolRecord>> SymbolTable::groupByFileId(
    const std::vector<SymbolRecord>& symbols) {
  std::map<std::uint32_t, std::vector<SymbolRecord>> grouped;
  for (const SymbolRecord& symbol : symbols) {
    grouped[symbol.fileId].push_back(symbol);
  }
  for (auto& [fileId, entries] : grouped) {
    (void)fileId;
    std::sort(entries.begin(), entries.end(), symbolLess);
  }
  return grouped;
}

}  // namespace ultra::ai
