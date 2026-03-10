#include "StructureExtractor.h"
#include <fstream>
#include <regex>
#include <sstream>

namespace ultra::ai {

namespace {

std::vector<std::string> extractWithPattern(const std::filesystem::path& file,
                                            const std::regex& re,
                                            std::size_t captureGroup) {
  std::vector<std::string> result;
  std::ifstream in(file);
  if (!in) return result;
  std::string line;
  while (std::getline(in, line)) {
    std::smatch m;
    if (std::regex_search(line, m, re) && m.size() > captureGroup) {
      std::string name = m[captureGroup].str();
      if (!name.empty()) result.push_back(name);
    }
  }
  return result;
}

std::string stripLineComment(std::string line) {
  auto pos = line.find("//");
  if (pos != std::string::npos) line.resize(pos);
  return line;
}

}  // namespace

std::vector<std::string> StructureExtractor::extractClasses(
    const std::filesystem::path& file) {
  std::vector<std::string> result;
  static const std::regex re(R"re(\bclass\s+(\w+)\s*[\{:])re");
  std::ifstream in(file);
  if (!in) return result;
  std::string line;
  while (std::getline(in, line)) {
    if (line.find("enum class") != std::string::npos) continue;
    std::smatch m;
    if (std::regex_search(line, m, re) && m.size() > 1) {
      result.push_back(m[1].str());
    }
  }
  return result;
}

std::vector<std::string> StructureExtractor::extractStructs(
    const std::filesystem::path& file) {
  static const std::regex re(R"re(\bstruct\s+(\w+)\s*[\{:])re");
  return extractWithPattern(file, re, 1);
}

std::vector<std::string> StructureExtractor::extractFunctions(
    const std::filesystem::path& file) {
  std::vector<std::string> result;
  std::ifstream in(file);
  if (!in) return result;
  static const std::regex re(
      R"re(\b(?:void|int|bool|char|short|long|float|double|size_t|std::\w+|\w+)\s+(\w+)\s*\([^)]*\)\s*(?:const)?\s*[;\{])re");
  std::string line;
  while (std::getline(in, line)) {
    line = stripLineComment(line);
    if (line.find("#define") != std::string::npos) continue;
    if (line.find("template") != std::string::npos) continue;
    std::smatch m;
    if (std::regex_search(line, m, re) && m.size() > 1) {
      std::string sig = m[1].str();
      if (sig != "if" && sig != "for" && sig != "while" && sig != "switch" &&
          sig != "catch" && sig != "return") {
        result.push_back(sig + "()");
      }
    }
  }
  return result;
}

}  // namespace ultra::ai
