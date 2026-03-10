#include "DiffParser.h"
#include <fstream>
#include <optional>

namespace ultra::patch {

namespace {

std::string stripPathPrefix(const std::string& path) {
  std::string p = path;
  if (p.size() >= 2 && (p[0] == 'a' || p[0] == 'b') &&
      (p[1] == '/' || p[1] == '\\')) {
    p = p.substr(2);
  }
  return p;
}

}  // namespace

std::vector<PatchOperation> DiffParser::parse(
    const std::filesystem::path& diffFile) {
  std::vector<PatchOperation> result;
  std::ifstream in(diffFile);
  if (!in) return result;
  std::string line;
  std::optional<PatchOperation> current;
  while (std::getline(in, line)) {
    if (line.size() >= 4 && line.substr(0, 4) == "--- ") {
      if (current.has_value() &&
          (!current->removedLines.empty() || !current->addedLines.empty())) {
        result.push_back(std::move(*current));
      }
      current = PatchOperation{};
      std::string pathPart = line.substr(4);
      auto tab = pathPart.find('\t');
      if (tab != std::string::npos) pathPart.resize(tab);
      current->targetFile = stripPathPrefix(pathPart);
    } else if (line.size() >= 4 && line.substr(0, 4) == "+++ ") {
      if (current.has_value()) {
        std::string pathPart = line.substr(4);
        auto tab = pathPart.find('\t');
        if (tab != std::string::npos) pathPart.resize(tab);
        current->targetFile = stripPathPrefix(pathPart);
      }
    } else if (line.size() >= 1 && line[0] == '-' && line.substr(0, 3) != "---") {
      if (current.has_value()) {
        current->removedLines.push_back(line.substr(1));
      }
    } else if (line.size() >= 1 && line[0] == '+' && line.substr(0, 3) != "+++") {
      if (current.has_value()) {
        current->addedLines.push_back(line.substr(1));
      }
    }
  }
  if (current.has_value() &&
      (!current->removedLines.empty() || !current->addedLines.empty())) {
    result.push_back(std::move(*current));
  }
  return result;
}

}  // namespace ultra::patch
