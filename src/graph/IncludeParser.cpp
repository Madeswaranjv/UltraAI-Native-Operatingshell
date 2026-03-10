#include "IncludeParser.h"
#include <fstream>
#include <regex>
#include <string>

namespace ultra::graph {

namespace {

const std::regex kLocalInclude(R"re(#include\s*"([^"]+)")re");

void stripLineComment(std::string& line) {
  auto pos = line.find("//");
  if (pos != std::string::npos) line.resize(pos);
}

bool stripBlockComments(std::string& line, bool& blockOpen) {
  std::string out;
  for (size_t i = 0; i < line.size(); ++i) {
    if (blockOpen) {
      if (i + 1 < line.size() && line[i] == '*' && line[i + 1] == '/') {
        blockOpen = false;
        i += 1;
      }
      continue;
    }
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '*') {
      blockOpen = true;
      i += 1;
      continue;
    }
    out += line[i];
  }
  line = std::move(out);
  return blockOpen;
}

}  // namespace

std::vector<std::string> IncludeParser::extractIncludes(
    const std::filesystem::path& file) {
  std::vector<std::string> result;
  std::ifstream in(file);
  if (!in) return result;
  std::string line;
  bool blockOpen = false;
  while (std::getline(in, line)) {
    if (stripBlockComments(line, blockOpen)) continue;
    stripLineComment(line);
    std::smatch m;
    if (std::regex_search(line, m, kLocalInclude)) result.push_back(m[1].str());
  }
  return result;
}

}  // namespace ultra::graph
