#include "ast/AstTypes.h"
#include "ast/AstExtractor.h"
#include "../ai/StructureExtractor.h"
#include <fstream>
#include <regex>
#include <string>

namespace ultra::ast {

namespace {

bool isReservedName(const std::string& s) {
  return s == "if" || s == "for" || s == "while" || s == "switch" ||
         s == "catch" || s == "return" || s == "main";
}

void extractWithPatternAndLine(const std::filesystem::path& file,
                               const std::regex& re,
                               std::size_t captureGroup,
                               std::vector<SymbolWithLine>& out) {
  std::ifstream in(file);
  if (!in) return;
  std::string line;
  std::size_t lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    std::smatch m;
    if (std::regex_search(line, m, re) && m.size() > captureGroup) {
      std::string name = m[captureGroup].str();
      if (!name.empty()) out.push_back({name, lineNo});
    }
  }
}

std::string stripLineComment(std::string line) {
  auto pos = line.find("//");
  if (pos != std::string::npos) line.resize(pos);
  return line;
}

FileStructure extractWithRegex(const std::filesystem::path& file) {
  FileStructure fs;
  std::ifstream in(file);
  if (!in) return fs;

  static const std::regex reClass(R"re(\bclass\s+(\w+)\s*[\{:])re");
  static const std::regex reStruct(R"re(\bstruct\s+(\w+)\s*[\{:])re");
  static const std::regex reNamespace(R"re(\bnamespace\s+(\w+)\s*[\{])re");
  static const std::regex reMethod(
      R"re(\b(?:void|int|bool|char|short|long|float|double|size_t|std::\w+|\w+)\s+(\w+)::(\w+)\s*\([^)]*\)\s*(?:const)?\s*[;\{])re");
  static const std::regex reFreeFunc(
      R"re(\b(?:void|int|bool|char|short|long|float|double|size_t|std::\w+|\w+)\s+(\w+)\s*\([^)]*\)\s*(?:const)?\s*[;\{])re");

  std::string line;
  std::size_t lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    if (line.find("enum class") != std::string::npos) continue;

    std::smatch m;
    if (std::regex_search(line, m, reClass) && m.size() > 1)
      fs.classes.push_back({m[1].str(), lineNo});
    if (std::regex_search(line, m, reStruct) && m.size() > 1)
      fs.structs.push_back({m[1].str(), lineNo});
    if (std::regex_search(line, m, reNamespace) && m.size() > 1)
      fs.namespaces.push_back({m[1].str(), lineNo});

    std::string stripped = stripLineComment(line);
    if (stripped.find("#define") != std::string::npos) continue;
    if (stripped.find("template") != std::string::npos) continue;

    if (std::regex_search(stripped, m, reMethod) && m.size() > 2) {
      std::string methodName = m[2].str();
      if (!isReservedName(methodName))
        fs.methods.push_back({methodName + "()", lineNo});
    }
    if (std::regex_search(stripped, m, reFreeFunc) && m.size() > 1) {
      std::string name = m[1].str();
      if (!isReservedName(name) && stripped.find("::") == std::string::npos)
        fs.freeFunctions.push_back({name + "()", lineNo});
    }
  }
  return fs;
}

}  // namespace

bool AstExtractor::isLibClangAvailable() {
#ifdef ULTRA_USE_LIBCLANG
  return true;
#else
  return false;
#endif
}

#ifdef ULTRA_USE_LIBCLANG
namespace ultra::ast {
extern FileStructure extractWithLibClang(const std::filesystem::path&);
}
#endif

FileStructure AstExtractor::extract(const std::filesystem::path& file) {
#ifdef ULTRA_USE_LIBCLANG
  return extractWithLibClang(file);
#else
  return extractWithRegex(file);
#endif
}

}  // namespace ultra::ast
