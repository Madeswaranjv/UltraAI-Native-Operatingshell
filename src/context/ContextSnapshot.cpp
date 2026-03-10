#include "context/ContextSnapshot.h"
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace ultra::context {

std::unordered_map<std::string, std::string> loadSnapshot(
    const std::filesystem::path& path) {
  std::unordered_map<std::string, std::string> out;
  try {
    if (!std::filesystem::exists(path) ||
        !std::filesystem::is_regular_file(path))
      return out;
    std::ifstream in(path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
      auto pos = line.find('|');
      if (pos == std::string::npos) continue;
      std::string p = line.substr(0, pos);
      std::string h = line.substr(pos + 1);
      if (!p.empty() && !h.empty()) out[p] = h;
    }
  } catch (...) {
    out.clear();
  }
  return out;
}

void saveSnapshot(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, std::string>& snapshot) {
  try {
    std::ofstream out(path);
    if (!out) return;
    std::vector<std::string> keys;
    keys.reserve(snapshot.size());
    for (const auto& kv : snapshot) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    for (const auto& p : keys) {
      if (p.find('|') != std::string::npos) continue;
      out << p << '|' << snapshot.at(p) << '\n';
    }
  } catch (...) {
  }
}

}  // namespace ultra::context
