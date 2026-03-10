#pragma once

#include "IProcessExecutor.h"

namespace ultra::platform {

class WindowsProcessExecutor : public IProcessExecutor {
 public:
  ProcessResult execute(const std::string& command) override;
  int run(const std::string& command) override;
};

}  // namespace ultra::platform
