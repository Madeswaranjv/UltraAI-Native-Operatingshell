#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <map>

namespace ultra::patch {

struct ApplyResult {
  bool success{false};
  std::size_t filesModified{0};
  int linesAdded{0};
  int linesRemoved{0};
  bool rollbackAvailable{false};
  std::string error{};
};

class PatchManager {
 public:
  PatchManager();

  // Unified diff format parsing and application
  ApplyResult applyUnifiedDiff(const std::filesystem::path& projectPath,
                               const std::string& diffText);

  // Exact line replace fallback
  ApplyResult applyExactLineReplace(const std::filesystem::path& projectPath,
                                    const std::string& file,
                                    int startLine, // 1-based inclusive
                                    int endLine,   // 1-based inclusive
                                    const std::string& replacement);
  
  bool rollback();

  // Remove all .ultra.bak backup files without restoring them.
  // Call after a confirmed successful apply to avoid bak accumulation.
  void clearBackups();

 private:
  void backupFile(const std::filesystem::path& file);
  void restoreFile(const std::filesystem::path& file);

  // Whitespace-normalised comparison for fuzzy context matching.
  // Trims leading/trailing space and collapses internal runs to single space.
  static std::string normalizeWS(const std::string& s);
  static bool linesMatch(const std::string& a, const std::string& b);

  std::map<std::filesystem::path, std::filesystem::path> backups_;
};

}  // namespace ultra::patch
