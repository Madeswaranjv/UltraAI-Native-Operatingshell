#pragma once

#include "BuildExecutor.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ultra::platform {
class IProcessExecutor;
}

namespace ultra::build {

class BuildEngine {
 public:
  BuildEngine();
  explicit BuildEngine(std::unique_ptr<ultra::platform::IProcessExecutor> executor);
  virtual ~BuildEngine();

  virtual int fullBuild(const std::filesystem::path& projectPath);
  virtual int incrementalBuild(const std::filesystem::path& projectPath,
                               const std::vector<std::string>& rebuildSet);

  /** [Experimental] Compile only changed .cpp with cl.exe then link;
   *  on failure falls back to full build. allCppCount = number of .cpp in project. */
  virtual int fastIncrementalBuild(const std::filesystem::path& projectPath,
                                   const std::vector<std::string>& rebuildSet,
                                   std::size_t allCppCount);

  [[nodiscard]] bool lastBuildRequired() const noexcept {
    return lastBuildResult_.buildRequired;
  }

  [[nodiscard]] bool lastBuildExecuted() const noexcept {
    return lastBuildResult_.buildExecuted;
  }

  [[nodiscard]] const BuildExecutionResult& lastBuildResult() const noexcept {
    return lastBuildResult_;
  }

 private:
  std::unique_ptr<ultra::platform::IProcessExecutor> executor_;
  BuildExecutionResult lastBuildResult_{};
};

}  // namespace ultra::build
