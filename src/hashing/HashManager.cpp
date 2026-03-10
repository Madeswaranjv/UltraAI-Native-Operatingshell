#include "HashManager.h"
#include "../graph/DependencyGraph.h"
#include "../scanner/FileInfo.h"
#include "utils/FileClassifier.h"
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include "../ai/Hashing.h"
//E:\Projects\Ultra\src\hashing\HashManager.cpp
namespace ultra::hashing {

namespace {

std::string hashToHex(std::size_t h) {
  std::ostringstream oss;
  oss << std::hex << h;
  return oss.str();
}

}  // namespace

HashManager::HashManager(const std::filesystem::path& dbPath)
    : dbPath_(dbPath) {}

void HashManager::load() {
  previousHashes_.clear();
  try {
    if (!std::filesystem::exists(dbPath_) ||
        !std::filesystem::is_regular_file(dbPath_)) {
      return;
    }
    std::ifstream in(dbPath_);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
      auto pos = line.find('|');
      if (pos == std::string::npos) continue;
      std::string path = line.substr(0, pos);
      std::string hash = line.substr(pos + 1);
      if (path.empty() || hash.empty()) continue;
      previousHashes_[path] = hash;
    }
  } catch (...) {
    previousHashes_.clear();
  }
}

void HashManager::save() const {
  try {
    std::ofstream out(dbPath_);
    if (!out) return;
    for (const auto& [path, hash] : currentHashes_) {
      out << path << '|' << hash << '\n';
    }
  } catch (...) {
  }
}

std::string HashManager::computeHash(const std::filesystem::path& file) const {
  try {
    ultra::ai::Sha256Hash digest;
    std::string err;
    if (!ultra::ai::sha256OfFile(file, digest, err)) {
      return "";
    }
    return ultra::ai::hashToHex(digest);
  } catch (...) {
    return "";
  }
}

std::vector<std::string> HashManager::detectChanges(
    const std::vector<ultra::scanner::FileInfo>& files) {
  currentHashes_.clear();
  std::vector<std::string> changed;
  for (const auto& f : files) {
    std::string key = f.path.lexically_normal().string();
    std::string hash = computeHash(f.path);
    if (hash.empty()) continue;
    currentHashes_[key] = hash;
    auto it = previousHashes_.find(key);
    if (it == previousHashes_.end() || it->second != hash) {
      changed.push_back(key);
    }
  }
  return changed;
}

std::vector<std::string> HashManager::detectChanges(
    const std::vector<ultra::scanner::FileInfo>& files,
    const ultra::graph::DependencyGraph& graph) {
  currentHashes_.clear();
  std::vector<std::string> changed;
  std::unordered_set<std::string> graphNodes;
  for (const std::string& n : graph.getNodes()) graphNodes.insert(n);
  for (const auto& f : files) {
    if (!ultra::utils::isContextSourceFile(f.path)) continue;
    std::string key = f.path.lexically_normal().string();
    if (graphNodes.find(key) == graphNodes.end()) continue;
    try {
      if (!std::filesystem::exists(f.path) ||
          !std::filesystem::is_regular_file(f.path))
        continue;
    } catch (...) {
      continue;
    }
    std::string hash = computeHash(f.path);
    if (hash.empty()) continue;
    currentHashes_[key] = hash;
    auto it = previousHashes_.find(key);
    if (it == previousHashes_.end() || it->second != hash) {
      changed.push_back(key);
    }
  }
  return changed;
}

}  // namespace ultra::hashing
