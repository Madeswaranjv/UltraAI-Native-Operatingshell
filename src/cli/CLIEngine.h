#pragma once

#include "UniversalCLI.h"
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
//E:\Projects\Ultra\src\cli\CLIEngine.h
namespace ultra::cli {

class CommandRouter;

class CLIEngine {
 public:
  explicit CLIEngine(CommandRouter& router);

  int run(int argc, char* argv[]);

  const std::vector<std::string>& currentArgs() const noexcept;

 private:
  struct ParsedCommand {
    std::string name;
    std::vector<std::string> args;
    bool valid{false};
  };

  ParsedCommand parse(int argc, char* argv[]) const;
  bool validate(const ParsedCommand& cmd) const;
  void registerHandlers();
  void handleAiVerify();

  void stripMetricsFlag(std::vector<std::string>& args);
  void printMetricsIfRequested(std::chrono::steady_clock::time_point start,
                               std::size_t filesProcessed = 0);

  CommandRouter& m_router;
  UniversalCLI m_universalCli;
  std::vector<std::string> m_currentArgs;
  std::filesystem::path m_projectRoot;
  int m_lastExitCode{0};
  bool m_metricsRequested{false};
};

}  // namespace ultra::cli
