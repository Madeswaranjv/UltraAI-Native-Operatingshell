#pragma once

#include "DependencyTable.h"
#include "FileRegistry.h"
#include "IntegrityManager.h"
#include "SymbolTable.h"

#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ultra::ai {

struct SymbolNode {
  std::string name;
  std::string definedIn;
  std::unordered_set<std::string> usedInFiles;
  double weight{0.0};
  double centrality{0.0};
};

struct RuntimeState {
  CoreIndex core{};
  std::vector<FileRecord> files;
  std::vector<SymbolRecord> symbols;
  DependencyTableData deps;
  std::map<std::uint32_t, std::vector<SemanticSymbolDependency>>
      semanticSymbolDepsByFileId;
  std::unordered_map<std::string, SymbolNode> symbolIndex;
};

}  // namespace ultra::ai
