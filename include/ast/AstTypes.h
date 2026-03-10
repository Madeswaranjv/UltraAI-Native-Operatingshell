#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ultra::ast {

struct SymbolWithLine {
  std::string name;
  std::size_t line{0};
};

struct FileStructure {
  std::vector<SymbolWithLine> classes;
  std::vector<SymbolWithLine> structs;
  std::vector<SymbolWithLine> methods;
  std::vector<SymbolWithLine> freeFunctions;
  std::vector<SymbolWithLine> namespaces;
};

}  // namespace ultra::ast
