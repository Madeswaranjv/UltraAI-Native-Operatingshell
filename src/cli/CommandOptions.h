#pragma once

#include <string>

namespace ultra::cli {

struct CommandOptions {
  bool release{false};
  bool debug{false};
  bool watch{false};
  bool parallel{false};
  bool force{false};
  bool clean{false};
  bool deep{false};
  bool verbose{false};
  bool dryRun{false};
  bool jsonOutput{false};
  std::string nativeArgs;
};

}  // namespace ultra::cli
