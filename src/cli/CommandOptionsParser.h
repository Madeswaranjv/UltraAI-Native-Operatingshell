#pragma once

#include "CommandOptions.h"
#include <cstddef>
#include <string>
#include <vector>

namespace ultra::cli {

struct CommandOptionsParseResult {
  bool ok{false};
  CommandOptions options;
  std::vector<std::string> positionalArgs;
  std::string error;
};

class CommandOptionsParser {
 public:
  static CommandOptionsParseResult parse(const std::vector<std::string>& args,
                                         bool allowPositional);
  static std::string joinForShell(const std::vector<std::string>& args,
                                  std::size_t startIndex = 0);

 private:
  static std::string quoteToken(const std::string& token);
};

}  // namespace ultra::cli
