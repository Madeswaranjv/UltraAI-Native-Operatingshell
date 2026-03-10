#pragma once

#include "DependencyTable.h"
#include "FileRegistry.h"
#include "IntegrityManager.h"
#include "SymbolTable.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

class BinaryIndexReader {
 public:
  static bool readCoreIndex(const std::filesystem::path& corePath,
                            CoreIndex& outCore,
                            std::string& error);
  static bool readFilesTable(const std::filesystem::path& tablePath,
                             std::uint32_t expectedSchemaVersion,
                             std::vector<FileRecord>& outFiles,
                             std::string& error);
  static bool readSymbolsTable(const std::filesystem::path& tablePath,
                               std::uint32_t expectedSchemaVersion,
                               std::vector<SymbolRecord>& outSymbols,
                               std::string& error);
  static bool readDependenciesTable(const std::filesystem::path& tablePath,
                                    std::uint32_t expectedSchemaVersion,
                                    DependencyTableData& outDeps,
                                    std::string& error);
};

}  // namespace ultra::ai

