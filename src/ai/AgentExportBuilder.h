#pragma once
//E:\Projects\Ultra\src\ai\AgentExportBuilder.h
#include "DependencyTable.h"
#include "FileRegistry.h"
#include "SymbolTable.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

class AgentExportBuilder {
 public:
  static bool writeAgentContext(const std::filesystem::path& outputPath,
                                const std::vector<FileRecord>& files,
                                const std::vector<SymbolRecord>& symbols,
                                const DependencyTableData& deps,
                                std::string& error);
};

}  // namespace ultra::ai

