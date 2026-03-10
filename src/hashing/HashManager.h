#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
//E:\Projects\Ultra\src\hashing\HashManager.h
namespace ultra::graph {
class DependencyGraph;
}
namespace ultra::scanner {
struct FileInfo;
}

namespace ultra::hashing {

class HashManager {
 public:
  explicit HashManager(const std::filesystem::path& dbPath);

  void load();
  void save() const;

  [[nodiscard]] std::string computeHash(
      const std::filesystem::path& file) const;

  std::vector<std::string> detectChanges(
      const std::vector<ultra::scanner::FileInfo>& files);

  /** Only consider files that are in the dependency graph and are context source. */
  std::vector<std::string> detectChanges(
      const std::vector<ultra::scanner::FileInfo>& files,
      const ultra::graph::DependencyGraph& graph);

 private:
  std::filesystem::path dbPath_;
  std::unordered_map<std::string, std::string> previousHashes_;
  std::unordered_map<std::string, std::string> currentHashes_;
};

}  // namespace ultra::hashing
