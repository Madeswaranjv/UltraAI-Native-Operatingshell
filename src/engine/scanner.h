#pragma once

#include "../ai/ChangeTracker.h"
#include "../ai/RuntimeState.h"
//E:\Projects\Ultra\src\engine\scanner.h
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::engine {

struct ChangeSet {
  std::set<std::string> added;
  std::set<std::string> modified;
  std::set<std::string> deleted;

  [[nodiscard]] bool empty() const {
    return added.empty() && modified.empty() && deleted.empty();
  }

  [[nodiscard]] std::size_t count() const {
    return added.size() + modified.size() + deleted.size();
  }
};

struct ScanOutput {
  std::vector<ai::FileRecord> files;
  std::vector<ai::SymbolRecord> symbols;
  ai::DependencyTableData deps;
  std::map<std::uint32_t, std::vector<ai::SemanticSymbolDependency>>
      semanticSymbolDepsByFileId;
  std::unordered_map<std::string, ai::SymbolNode> symbolIndex;
  ChangeSet changeSet;
  std::vector<ai::ChangeLogRecord> changesForLog;
};

class Scanner {
 public:
  explicit Scanner(std::filesystem::path projectRoot);

  [[nodiscard]] std::size_t countFiles() const;

  bool fullScanParallel(ScanOutput& output, std::string& error) const;
  bool incrementalAdd(ai::RuntimeState& state,
                      const std::string& path,
                      ScanOutput& output,
                      std::string& error) const;
  bool incrementalRemove(ai::RuntimeState& state,
                         const std::string& path,
                         ScanOutput& output,
                         std::string& error) const;
  bool incrementalModify(ai::RuntimeState& state,
                         const std::string& path,
                         ScanOutput& output,
                         std::string& error) const;

 private:
  bool resolveDependencyReference(
      const std::string& currentFilePath,
      const std::string& reference,
      const std::map<std::string, ai::FileRecord>& currentFilesByPath,
      std::string& resolvedPath) const;

  std::filesystem::path projectRoot_;
};

}  // namespace ultra::engine
