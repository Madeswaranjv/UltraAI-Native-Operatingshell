#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ultra::memory {

struct SymbolHistoryEntry {
  std::uint64_t version{0U};
  std::string stableIdentity;
  std::string nodeId;
  std::string symbolName;
  std::string signature;
  std::string changeType;
};

class SemanticMemory {
 public:
  void trackSymbolEvolution(const std::string& nodeId,
                            const std::string& symbolName,
                            const std::string& signature,
                            const std::string& changeType,
                            std::uint64_t version,
                            const std::string& predecessorNodeId = {});

  [[nodiscard]] std::string resolveStableIdentity(
      const std::string& nodeId) const;
  [[nodiscard]] std::vector<SymbolHistoryEntry> getSymbolHistory(
      const std::string& stableIdentity) const;

  [[nodiscard]] bool persistToFile(const std::filesystem::path& path) const;
  [[nodiscard]] bool loadFromFile(const std::filesystem::path& path);

 private:
  static constexpr std::uint32_t kSchemaVersion = 1U;
  static std::string makeStableIdentity(const std::string& nodeId);

  mutable std::shared_mutex mutex_;
  std::map<std::string, std::string> stableIdentityByNode_;
  std::map<std::string, std::vector<SymbolHistoryEntry>> historyByStableIdentity_;
};

}  // namespace ultra::memory
