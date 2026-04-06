#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <cstdint>
#include <vector>

namespace ultra::runtime::cognitive::tool_router {

class ToolRouter {
 public:
  struct CommandResult {
    int exitCode{1};
    std::string output;
    std::string error;
    std::int64_t durationMs{0};
  };

  using CommandRunner = std::function<CommandResult(const std::string&)>;
  using FileFallbackReader =
      std::function<std::optional<std::string>(const std::string&)>;

  ToolRouter();
  explicit ToolRouter(CommandRunner commandRunner,
                      FileFallbackReader fallbackReader = {});

  std::string route_and_execute(
      const std::string& tool,
      const std::map<std::string, std::string>& args);

 private:
  std::string run_ultra_command(const std::string& cmd);
  std::optional<std::string> execute_local_tool(
      const std::string& tool,
      const std::map<std::string, std::string>& args);
  std::string execute_read_file(
      const std::map<std::string, std::string>& args) const;
  std::string execute_write_file(
      const std::map<std::string, std::string>& args) const;
  std::string execute_append_file(
      const std::map<std::string, std::string>& args) const;
  std::string execute_delete_file(
      const std::map<std::string, std::string>& args) const;
  std::string execute_list_dir(
      const std::map<std::string, std::string>& args) const;
  std::string execute_search_files(
      const std::map<std::string, std::string>& args) const;
  std::string execute_apply_patch(
      const std::map<std::string, std::string>& args) const;
  std::string execute_run_command(
      const std::map<std::string, std::string>& args) const;

  [[nodiscard]] static std::optional<std::string> find_argument(
      const std::map<std::string, std::string>& args,
      const std::string& key);
  [[nodiscard]] static std::string quote_argument(const std::string& raw);
  [[nodiscard]] static bool parse_bool_argument(
      const std::map<std::string, std::string>& args,
      const std::string& key,
      bool defaultValue);
  [[nodiscard]] static int parse_int_argument(
      const std::map<std::string, std::string>& args,
      const std::string& key,
      int defaultValue,
      int minimumValue);
  [[nodiscard]] static std::string error_json(
      std::string tool,
      std::string message);
  [[nodiscard]] static std::string success_json(const std::string& tool,
                                                const std::string& payload);
  [[nodiscard]] static std::string limit_output(std::string text,
                                                std::size_t limitBytes);
  [[nodiscard]] static bool is_excluded_directory(
      const std::filesystem::path& path);
  [[nodiscard]] std::optional<std::string> build_command(
      const std::string& tool,
      const std::map<std::string, std::string>& args) const;
  [[nodiscard]] std::optional<std::string> fallback_read_source(
      const std::map<std::string, std::string>& args) const;

  CommandRunner commandRunner_;
  FileFallbackReader fallbackReader_;
  bool lastCommandSucceeded_{false};
  int lastExitCode_{-1};
};

}  // namespace ultra::runtime::cognitive::tool_router
