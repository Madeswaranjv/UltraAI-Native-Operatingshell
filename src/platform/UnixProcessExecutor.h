#pragma once

#include "IProcessExecutor.h"

namespace ultra::platform {

/// Unix/macOS process executor using POSIX APIs.
class UnixProcessExecutor : public IProcessExecutor {
 public:
  ProcessResult execute(const std::string& command) override;
  int run(const std::string& command) override;
};

}  // namespace ultra::platform
