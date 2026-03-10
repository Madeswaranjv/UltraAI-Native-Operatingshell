#pragma once

#include <external/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ultra::ai {

struct AiStatusSnapshot {
  bool runtimeActive{false};
  bool indexPresent{false};
  bool integrityOk{false};
  std::size_t filesIndexed{0};
  std::size_t symbolsIndexed{0};
  std::size_t dependenciesIndexed{0};
  std::size_t pendingChanges{0};
  std::uint32_t schemaVersion{0U};
  std::uint32_t indexVersion{0U};
};

class AiRuntimeManager {
 public:
  explicit AiRuntimeManager(std::filesystem::path projectRoot);

  int wakeAi(bool verbose = true);
  int rebuildAi(bool verbose = true);
  int aiStatus(bool verbose = true);
  int aiVerify(bool verbose = true);
  bool contextDiff(nlohmann::json& payloadOut, std::string& error);
  void silentIncrementalUpdate();
  static bool requestDaemon(const std::filesystem::path& projectRoot,
                            const std::string& command,
                            nlohmann::json& response,
                            std::string& error);
  static bool requestDaemon(const std::filesystem::path& projectRoot,
                            const std::string& command,
                            const nlohmann::json& requestPayload,
                            nlohmann::json& response,
                            std::string& error);

 private:
  static bool daemonChildModeEnabled();
  int runDaemonLoop(bool verbose);

  std::filesystem::path projectRoot_;
};

}  // namespace ultra::ai

