#include "utils/FileClassifier.h"
#include <algorithm>
#include <string>

namespace ultra::utils {

namespace {

const std::string kUltraPrefix(".ultra");
const std::string kUltraFilePrefix("ultra_");

bool hasArtifactExtension(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  if (ext.empty()) return false;
  return ext == ".json" || ext == ".dot" || ext == ".log";
}

bool hasArtifactName(const std::filesystem::path& p) {
  if (!p.has_filename()) return false;
  std::string name = p.filename().string();
  if (name.size() >= kUltraPrefix.size() &&
      name.compare(0, kUltraPrefix.size(), kUltraPrefix) == 0) {
    return true;
  }
  if (name.size() >= kUltraFilePrefix.size() &&
      name.compare(0, kUltraFilePrefix.size(), kUltraFilePrefix) == 0) {
    return true;
  }
  return false;
}

}  // namespace

bool isToolArtifact(const std::filesystem::path& p) {
  try {
    if (!p.has_filename()) return false;
    if (hasArtifactName(p)) return true;
    if (hasArtifactExtension(p)) return true;
    return false;
  } catch (...) {
    return false;
  }
}

bool isContextSourceFile(const std::filesystem::path& p) {
  try {
    if (isToolArtifact(p)) return false;
    std::string ext = p.extension().string();
    return ext == ".cpp" || ext == ".h" || ext == ".hpp";
  } catch (...) {
    return false;
  }
}

bool isParsableSourceOrHeader(const std::filesystem::path& p) {
  try {
    if (isToolArtifact(p)) return false;
    std::string ext = p.extension().string();
    return ext == ".cpp" || ext == ".h" || ext == ".hpp";
  } catch (...) {
    return false;
  }
}

}  // namespace ultra::utils
