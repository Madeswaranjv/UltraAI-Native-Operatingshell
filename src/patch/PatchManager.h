#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace ultra::build {
class BuildEngine;
}
namespace ultra::patch {
struct PatchOperation;
}

namespace ultra::patch {

struct ApplyResult {
  bool success{false};
  std::size_t filesModified{0};
};

class PatchManager {
 public:
  explicit PatchManager(ultra::build::BuildEngine& buildEngine);

  ApplyResult applyPatch(const std::filesystem::path& projectPath,
                         const std::filesystem::path& diffFile);

 private:
  void backupFile(const std::filesystem::path& file);
  void restoreFile(const std::filesystem::path& file);
  bool applyOperation(const std::filesystem::path& projectPath,
                      const PatchOperation& op);

  ultra::build::BuildEngine& buildEngine_;
};

}  // namespace ultra::patch
