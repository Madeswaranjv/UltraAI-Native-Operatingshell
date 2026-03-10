#pragma once

#include "../../ai/DependencyTable.h"
#include "../../ai/FileRegistry.h"
#include "../../ai/Hashing.h"
#include "../../ai/SemanticExtractor.h"
#include "../../ai/SymbolTable.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ultra::engine::parallel {

using DependencyResolver = std::function<bool(
    const std::string& currentFilePath,
    const std::string& reference,
    const std::map<std::string, ai::FileRecord>& currentFilesByPath,
    std::string& resolvedPath)>;

struct ParallelScanResult {
  std::vector<ai::FileRecord> files;
  std::vector<ai::SymbolRecord> symbols;
  ai::DependencyTableData deps;
  std::map<std::uint32_t, std::vector<ai::SemanticSymbolDependency>>
      semanticSymbolDepsByFileId;
};

class ParallelScanner {
 public:
  explicit ParallelScanner(std::filesystem::path projectRoot);

  bool runFullScan(const DependencyResolver& dependencyResolver,
                   ParallelScanResult& output,
                   std::string& error) const;

 private:
  std::filesystem::path projectRoot_;
};

}  // namespace ultra::engine::parallel
