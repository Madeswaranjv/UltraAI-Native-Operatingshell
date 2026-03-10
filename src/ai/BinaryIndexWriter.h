#pragma once

#include "DependencyTable.h"
#include "FileRegistry.h"
#include "IntegrityManager.h"
#include "SymbolTable.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

class BinaryIndexWriter {
 public:
  static constexpr std::uint32_t kFilesMagic = 0x53454C49U;    // "ILES"
  static constexpr std::uint32_t kSymbolsMagic = 0x424D5953U;  // "SYMB"
  static constexpr std::uint32_t kDepsMagic = 0x53504544U;     // "DEPS"

  static bool writeCoreIndex(const std::filesystem::path& corePath,
                             const CoreIndex& core,
                             std::string& error);
  static bool writeFilesTable(const std::filesystem::path& tablePath,
                              std::uint32_t schemaVersion,
                              const std::vector<FileRecord>& files,
                              std::string& error);
  static bool writeSymbolsTable(const std::filesystem::path& tablePath,
                                std::uint32_t schemaVersion,
                                const std::vector<SymbolRecord>& symbols,
                                std::string& error);
  static bool writeDependenciesTable(const std::filesystem::path& tablePath,
                                     std::uint32_t schemaVersion,
                                     const DependencyTableData& deps,
                                     std::string& error);
};

}  // namespace ultra::ai

