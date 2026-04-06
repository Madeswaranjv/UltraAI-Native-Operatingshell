#include "ToolRouter.h"

#include "../../../core/Logger.h"
#include "../../../platform/UnixProcessExecutor.h"
#include "../../../platform/WindowsProcessExecutor.h"

#include <external/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ultra::runtime::cognitive::tool_router {

namespace {

constexpr std::size_t kMaxSearchMatches = 100U;
constexpr std::size_t kMaxCommandOutputBytes = 64U * 1024U;

std::string quoteForShell(const std::string& raw) {
  std::string escaped;
  escaped.reserve(raw.size() + 2U);
  escaped.push_back('"');
  for (const char ch : raw) {
    if (ch == '"') {
      escaped += "\\\"";
      continue;
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

std::string commandWithWorkingDirectory(const std::filesystem::path& cwd,
                                        const std::string& command) {
#if defined(_WIN32)
  return "cd /d " + quoteForShell(cwd.string()) + " && " + command;
#else
  return "cd " + quoteForShell(cwd.string()) + " && " + command;
#endif
}

std::string lowerAscii(std::string value) {
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

std::string slurpFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }

  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

bool startsWithErrorMarker(const std::string& value) {
  return value.rfind("[ERROR]", 0U) == 0U || value.rfind("ERROR:", 0U) == 0U;
}

bool seemsBinaryText(const std::string& content) {
  return content.find('\0') != std::string::npos;
}

std::string canonicalToolName(std::string tool) {
  // Route through the canonical underscore tool namespace while
  // accepting legacy hyphenated aliases.
  std::replace(tool.begin(), tool.end(), '-', '_');
  return tool;
}

std::string formatToolResponse(const nlohmann::ordered_json& payload) {
  const std::string dumped = payload.dump();
  if (!payload.value("ok", true)) {
    return "ERROR: " + dumped;
  }
  return dumped;
}

std::optional<nlohmann::ordered_json> parseTrailingJsonLine(
    const std::string& text) {
  std::size_t candidateEnd = text.size();
  while (candidateEnd > 0U &&
         (text[candidateEnd - 1U] == '\n' || text[candidateEnd - 1U] == '\r' ||
          text[candidateEnd - 1U] == ' ' || text[candidateEnd - 1U] == '\t')) {
    --candidateEnd;
  }
  if (candidateEnd == 0U) {
    return std::nullopt;
  }

  while (candidateEnd > 0U) {
    std::size_t lineStart = text.rfind('\n', candidateEnd - 1U);
    lineStart = lineStart == std::string::npos ? 0U : lineStart + 1U;
    while (lineStart < candidateEnd &&
           (text[lineStart] == '\r' || text[lineStart] == ' ' ||
            text[lineStart] == '\t')) {
      ++lineStart;
    }
    if (lineStart < candidateEnd && text[lineStart] == '{') {
      try {
        nlohmann::ordered_json parsed = nlohmann::ordered_json::parse(
            text.substr(lineStart, candidateEnd - lineStart));
        if (parsed.is_object()) {
          return parsed;
        }
      } catch (...) {
      }
    }

    if (lineStart == 0U) {
      break;
    }
    candidateEnd = lineStart - 1U;
    while (candidateEnd > 0U &&
           (text[candidateEnd - 1U] == '\n' || text[candidateEnd - 1U] == '\r')) {
      --candidateEnd;
    }
  }

  return std::nullopt;
}

ToolRouter::CommandResult runWithPlatformExecutor(const std::string& command) {
  ToolRouter::CommandResult result;

  std::error_code ec;
  const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path stdoutPath =
      ec ? std::filesystem::path("ultra_tool_router_stdout.log")
         : tempDir / ("ultra_tool_router_stdout_" + std::to_string(suffix) + ".log");
  const std::filesystem::path stderrPath =
      ec ? std::filesystem::path("ultra_tool_router_stderr.log")
         : tempDir / ("ultra_tool_router_stderr_" + std::to_string(suffix) + ".log");

  const std::string redirected = command +
                                 " > " + quoteForShell(stdoutPath.string()) +
                                 " 2> " + quoteForShell(stderrPath.string());

#if defined(_WIN32)
  ultra::platform::WindowsProcessExecutor executor;
  const ultra::platform::ProcessResult processResult =
      executor.execute(redirected);
#else
  ultra::platform::UnixProcessExecutor executor;
  const ultra::platform::ProcessResult processResult =
      executor.execute(redirected);
#endif

  result.exitCode = processResult.exitCode;
  result.durationMs = processResult.durationMs;
  result.output = slurpFile(stdoutPath);
  result.error = slurpFile(stderrPath);

  std::error_code removeError;
  std::filesystem::remove(stdoutPath, removeError);
  removeError.clear();
  std::filesystem::remove(stderrPath, removeError);
  return result;
}

std::optional<std::string> defaultFileFallbackReader(const std::string& target) {
  const std::filesystem::path path = std::filesystem::path(target);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return std::nullopt;
  }

  const std::string content = slurpFile(path);
  if (content.empty() && std::filesystem::file_size(path, ec) > 0U && !ec) {
    return std::nullopt;
  }
  return content;
}

}  // namespace

ToolRouter::ToolRouter()
    : commandRunner_(runWithPlatformExecutor),
      fallbackReader_(defaultFileFallbackReader) {}

ToolRouter::ToolRouter(CommandRunner commandRunner,
                       FileFallbackReader fallbackReader)
    : commandRunner_(commandRunner ? std::move(commandRunner)
                                   : runWithPlatformExecutor),
      fallbackReader_(fallbackReader ? std::move(fallbackReader)
                                     : defaultFileFallbackReader) {}

std::string ToolRouter::route_and_execute(
    const std::string& tool,
    const std::map<std::string, std::string>& args) {
  const std::string dispatchTool = canonicalToolName(tool);

  if (const std::optional<std::string> localResult =
          execute_local_tool(dispatchTool, args);
      localResult.has_value()) {
    return *localResult;
  }

  const std::optional<std::string> command = build_command(dispatchTool, args);
  if (!command.has_value()) {
    return "ERROR: invalid tool request or missing arguments for tool '" + tool +
           "'.";
  }

  core::Logger::info(core::LogCategory::General,
                     "ToolRouter executing Ultra command: " + *command);
  const std::string firstAttempt = run_ultra_command(*command);
  if (lastCommandSucceeded_) {
    return firstAttempt;
  }

  core::Logger::warning(
      core::LogCategory::General,
      "ToolRouter retrying Ultra command after failure for tool '" + tool + "'.");
  const std::string secondAttempt = run_ultra_command(*command);
  if (lastCommandSucceeded_) {
    return secondAttempt;
  }

  if (dispatchTool == "read_source") {
    if (const std::optional<std::string> fallback = fallback_read_source(args);
        fallback.has_value()) {
      core::Logger::warning(
          core::LogCategory::General,
          "ToolRouter using fallback file read for read_source.");
      return *fallback;
    }
  }

  const std::string failureOutput =
      secondAttempt.empty() ? firstAttempt : secondAttempt;
  if (failureOutput.empty()) {
    return "ERROR: Ultra command failed for tool '" + tool +
           "' after retry (exit_code=" + std::to_string(lastExitCode_) + ").";
  }
  return "ERROR: Ultra command failed for tool '" + tool +
         "' after retry (exit_code=" + std::to_string(lastExitCode_) +
         ").\n" + failureOutput;
}

std::string ToolRouter::run_ultra_command(const std::string& cmd) {
  const CommandResult result =
      commandRunner_ ? commandRunner_(cmd) : CommandResult{};

  std::string combined = result.output;
  if (!result.error.empty()) {
    if (!combined.empty() && combined.back() != '\n') {
      combined.push_back('\n');
    }
    combined += result.error;
  }

  lastExitCode_ = result.exitCode;
  lastCommandSucceeded_ =
      result.exitCode == 0 && !startsWithErrorMarker(combined);
  return combined;
}

std::optional<std::string> ToolRouter::execute_local_tool(
    const std::string& tool,
    const std::map<std::string, std::string>& args) {
  if (tool == "read_file") {
    return execute_read_file(args);
  }
  if (tool == "write_file") {
    return execute_write_file(args);
  }
  if (tool == "append_file") {
    return execute_append_file(args);
  }
  if (tool == "delete_file") {
    return execute_delete_file(args);
  }
  if (tool == "list_dir") {
    return execute_list_dir(args);
  }
  if (tool == "search_files") {
    return execute_search_files(args);
  }
  if (tool == "apply_patch") {
    return execute_apply_patch(args);
  }
  if (tool == "run_command") {
    return execute_run_command(args);
  }
  return std::nullopt;
}

std::string ToolRouter::execute_read_file(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["content"] = "";
  payload["size_bytes"] = 0;
  payload["ok"] = false;
  payload["error"] = "";

  const std::optional<std::string> pathArg = find_argument(args, "path");
  if (!pathArg.has_value()) {
    payload["error"] = "missing required argument: path";
    return formatToolResponse(payload);
  }

  const std::filesystem::path path(*pathArg);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    payload["error"] = "file not found: " + path.generic_string();
    return formatToolResponse(payload);
  }
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    payload["error"] = "not a regular file: " + path.generic_string();
    return formatToolResponse(payload);
  }

  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    payload["error"] = "failed to open file: " + path.generic_string();
    return formatToolResponse(payload);
  }

  std::ostringstream stream;
  stream << input.rdbuf();
  const std::string content = stream.str();
  if (seemsBinaryText(content)) {
    payload["error"] = "binary file is not supported";
    return formatToolResponse(payload);
  }

  payload["content"] = content;
  payload["size_bytes"] = static_cast<std::int64_t>(content.size());
  payload["ok"] = true;
  return formatToolResponse(payload);
}

std::string ToolRouter::execute_write_file(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["ok"] = false;
  payload["error"] = "";

  const std::optional<std::string> pathArg = find_argument(args, "path");
  if (!pathArg.has_value()) {
    payload["error"] = "missing required argument: path";
    return formatToolResponse(payload);
  }
  const std::optional<std::string> contentArg = find_argument(args, "content");
  const std::string content = contentArg.value_or(std::string{});

  const bool createDirs = parse_bool_argument(args, "create_dirs", false);
  const std::filesystem::path path(*pathArg);
  const std::filesystem::path parent = path.parent_path();

  std::error_code ec;
  if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
    if (!createDirs) {
      payload["error"] = "parent directory does not exist: " + parent.generic_string();
      return formatToolResponse(payload);
    }
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      payload["error"] = "failed to create parent directories: " + ec.message();
      return formatToolResponse(payload);
    }
  }

  // TODO(governance): enforce protectedPaths/forbiddenActions checks before writes.
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path tmpPath =
      path.parent_path() /
      (path.filename().string() + ".tmp." + std::to_string(suffix));

  {
    std::ofstream output(tmpPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      payload["error"] = "failed to open temporary file for write";
      return formatToolResponse(payload);
    }
    output << content;
    if (!output.good()) {
      payload["error"] = "failed while writing temporary file";
      std::error_code removeError;
      std::filesystem::remove(tmpPath, removeError);
      return formatToolResponse(payload);
    }
  }

  ec.clear();
  std::filesystem::rename(tmpPath, path, ec);
  if (ec) {
    std::error_code removeExistingError;
    std::filesystem::remove(path, removeExistingError);
    ec.clear();
    std::filesystem::rename(tmpPath, path, ec);
  }
  if (ec) {
    payload["error"] = "atomic rename failed: " + ec.message();
    std::error_code removeTmpError;
    std::filesystem::remove(tmpPath, removeTmpError);
    return formatToolResponse(payload);
  }

  payload["ok"] = true;
  return formatToolResponse(payload);
}

std::string ToolRouter::execute_append_file(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["ok"] = false;
  payload["error"] = "";

  const std::optional<std::string> pathArg = find_argument(args, "path");
  if (!pathArg.has_value()) {
    payload["error"] = "missing required argument: path";
    return formatToolResponse(payload);
  }
  const std::optional<std::string> contentArg = find_argument(args, "content");
  if (!contentArg.has_value()) {
    payload["error"] = "missing required argument: content";
    return formatToolResponse(payload);
  }

  const std::filesystem::path path(*pathArg);
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output.is_open()) {
    payload["error"] = "failed to open file for append: " + path.generic_string();
    return formatToolResponse(payload);
  }
  output << *contentArg;
  if (!output.good()) {
    payload["error"] = "append failed: " + path.generic_string();
    return formatToolResponse(payload);
  }

  payload["ok"] = true;
  return formatToolResponse(payload);
}

std::string ToolRouter::execute_delete_file(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["ok"] = false;
  payload["error"] = "";

  const std::optional<std::string> pathArg = find_argument(args, "path");
  if (!pathArg.has_value()) {
    payload["error"] = "missing required argument: path";
    return formatToolResponse(payload);
  }

  const std::filesystem::path path(*pathArg);
  std::error_code ec;
  const bool removed = std::filesystem::remove(path, ec);
  if (ec) {
    payload["error"] = "delete failed: " + ec.message();
    return formatToolResponse(payload);
  }
  if (!removed) {
    payload["error"] = "file not found: " + path.generic_string();
    return formatToolResponse(payload);
  }

  payload["ok"] = true;
  return formatToolResponse(payload);
}

std::string ToolRouter::execute_list_dir(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["entries"] = nlohmann::ordered_json::array();
  payload["ok"] = false;
  payload["error"] = "";

  const std::filesystem::path root(
      find_argument(args, "path").value_or(std::string{"."}));
  const bool recursive = parse_bool_argument(args, "recursive", false);

  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec ||
      !std::filesystem::is_directory(root, ec)) {
    payload["error"] = "path is not a directory: " + root.generic_string();
    return formatToolResponse(payload);
  }

  std::vector<nlohmann::ordered_json> entries;
  if (recursive) {
    std::filesystem::recursive_directory_iterator it(root, ec);
    std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end) {
      const std::filesystem::path entryPath = it->path();
      if (it->is_directory(ec) && is_excluded_directory(entryPath)) {
        it.disable_recursion_pending();
        ++it;
        continue;
      }
      if (is_excluded_directory(entryPath)) {
        ++it;
        continue;
      }

      nlohmann::ordered_json item;
      std::error_code relError;
      const std::filesystem::path relative =
          std::filesystem::relative(entryPath, root, relError);
      item["name"] = relError ? entryPath.generic_string() : relative.generic_string();
      item["type"] = it->is_directory(ec) ? "dir" : "file";
      item["size_bytes"] =
          it->is_regular_file(ec) ? static_cast<std::int64_t>(it->file_size(ec)) : 0;
      entries.push_back(std::move(item));
      ++it;
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
      if (ec) {
        break;
      }
      if (is_excluded_directory(entry.path())) {
        continue;
      }
      nlohmann::ordered_json item;
      item["name"] = entry.path().filename().generic_string();
      item["type"] = entry.is_directory(ec) ? "dir" : "file";
      item["size_bytes"] =
          entry.is_regular_file(ec) ? static_cast<std::int64_t>(entry.file_size(ec)) : 0;
      entries.push_back(std::move(item));
    }
  }

  if (ec) {
    payload["error"] = "directory traversal failed: " + ec.message();
    return formatToolResponse(payload);
  }

  std::sort(entries.begin(), entries.end(),
            [](const nlohmann::ordered_json& left,
               const nlohmann::ordered_json& right) {
              return left.value("name", std::string{}) <
                     right.value("name", std::string{});
            });
  for (auto& entry : entries) {
    payload["entries"].push_back(std::move(entry));
  }
  payload["ok"] = true;
  return formatToolResponse(payload);
}

std::string ToolRouter::execute_search_files(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["matches"] = nlohmann::ordered_json::array();
  payload["count"] = 0;
  payload["ok"] = false;
  payload["error"] = "";

  const std::optional<std::string> patternArg = find_argument(args, "pattern");
  if (!patternArg.has_value()) {
    payload["error"] = "missing required argument: pattern";
    return formatToolResponse(payload);
  }
  const std::filesystem::path root(
      find_argument(args, "path").value_or(std::string{"."}));
  const bool caseSensitive = parse_bool_argument(args, "case_sensitive", false);

  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec ||
      !std::filesystem::is_directory(root, ec)) {
    payload["error"] = "path is not a directory: " + root.generic_string();
    return formatToolResponse(payload);
  }

  const std::string pattern = *patternArg;
  const std::string normalizedPattern =
      caseSensitive ? pattern : lowerAscii(pattern);
  std::size_t totalMatches = 0U;

  std::filesystem::recursive_directory_iterator it(root, ec);
  std::filesystem::recursive_directory_iterator end;
  while (!ec && it != end && totalMatches < kMaxSearchMatches) {
    const auto& entry = *it;
    const std::filesystem::path entryPath = entry.path();
    if (entry.is_directory(ec) && is_excluded_directory(entryPath)) {
      it.disable_recursion_pending();
      ++it;
      continue;
    }
    if (!entry.is_regular_file(ec) || is_excluded_directory(entryPath)) {
      ++it;
      continue;
    }

    std::ifstream input(entryPath);
    if (!input.is_open()) {
      ++it;
      continue;
    }

    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line) && totalMatches < kMaxSearchMatches) {
      ++lineNumber;
      const std::string haystack = caseSensitive ? line : lowerAscii(line);
      if (haystack.find(normalizedPattern) == std::string::npos) {
        continue;
      }

      nlohmann::ordered_json match;
      std::error_code relError;
      const std::filesystem::path relative =
          std::filesystem::relative(entryPath, root, relError);
      match["file"] = relError ? entryPath.generic_string() : relative.generic_string();
      match["line"] = lineNumber;
      match["content"] = line;
      payload["matches"].push_back(std::move(match));
      ++totalMatches;
    }
    ++it;
  }

  if (ec) {
    payload["error"] = "search traversal failed: " + ec.message();
    return formatToolResponse(payload);
  }

  payload["count"] = static_cast<std::int64_t>(totalMatches);
  payload["ok"] = true;
  return formatToolResponse(payload);
}

std::string ToolRouter::execute_apply_patch(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["ok"] = false;
  payload["applied"] = false;
  payload["file_verified"] = false;
  payload["exit_code"] = 1;
  payload["stdout"] = "";
  payload["stderr"] = "";
  payload["diff_bytes"] = 0;

  const std::string diffText =
      find_argument(args, "diff")
          .value_or(find_argument(args, "changes").value_or(std::string{}));
  if (diffText.empty()) {
    payload["error"] = "missing required argument: diff or changes";
    return formatToolResponse(payload);
  }

  const std::filesystem::path projectPath(
      find_argument(args, "project_path")
          .value_or(std::filesystem::current_path().generic_string()));
  payload["project_path"] = projectPath.generic_string();
  if (const std::optional<std::string> fileArg = find_argument(args, "file");
      fileArg.has_value()) {
    payload["file"] = *fileArg;
  }
  if (const std::optional<std::string> pathArg = find_argument(args, "path");
      pathArg.has_value()) {
    payload["path"] = *pathArg;
  }

  std::error_code ec;
  if (!std::filesystem::exists(projectPath, ec) || ec ||
      !std::filesystem::is_directory(projectPath, ec)) {
    payload["error"] = "project_path is not a directory: " +
                         projectPath.generic_string();
    return formatToolResponse(payload);
  }

  const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path diffPath =
      (ec ? std::filesystem::path("ultra_apply_patch.diff")
          : tempDir / ("ultra_apply_patch_" + std::to_string(suffix) + ".diff"));

  {
    std::ofstream output(diffPath, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      payload["error"] = "failed to create temporary diff file";
      return formatToolResponse(payload);
    }
    output << diffText;
    if (!output.good()) {
      payload["error"] = "failed while writing temporary diff file";
      std::error_code removeError;
      std::filesystem::remove(diffPath, removeError);
      return formatToolResponse(payload);
    }
  }

  payload["diff_bytes"] = static_cast<std::int64_t>(diffText.size());
  const std::string command = commandWithWorkingDirectory(
      projectPath,
      "ultra apply_patch " + quote_argument(projectPath.string()) + " " +
          quote_argument(diffPath.string()));
  const CommandResult commandResult =
      commandRunner_ ? commandRunner_(command) : CommandResult{};

  const std::string stdoutText =
      limit_output(commandResult.output, kMaxCommandOutputBytes);
  const std::string stderrText =
      limit_output(commandResult.error, kMaxCommandOutputBytes);
  std::error_code removeError;
  std::filesystem::remove(diffPath, removeError);

  payload["stdout"] = stdoutText;
  payload["stderr"] = stderrText;
  payload["exit_code"] = commandResult.exitCode;
  payload["duration_ms"] = commandResult.durationMs;
  payload["applied"] = commandResult.exitCode == 0;
  payload["ok"] = commandResult.exitCode == 0;

  if (const std::optional<nlohmann::ordered_json> summary =
          parseTrailingJsonLine(stdoutText);
      summary.has_value()) {
    payload["patch_result"] = *summary;
    if (summary->contains("applied") && (*summary)["applied"].is_boolean()) {
      payload["applied"] = (*summary)["applied"].get<bool>();
    }
    if (summary->contains("file_verified") &&
        (*summary)["file_verified"].is_boolean()) {
      payload["file_verified"] = (*summary)["file_verified"].get<bool>();
    }
    if (summary->contains("ok") && (*summary)["ok"].is_boolean()) {
      payload["ok"] =
          payload["ok"].get<bool>() && (*summary)["ok"].get<bool>();
    }
    if (summary->contains("build_required")) {
      payload["build_required"] = (*summary)["build_required"];
    }
    if (summary->contains("build_executed")) {
      payload["build_executed"] = (*summary)["build_executed"];
    }
    if (summary->contains("build_exit_code")) {
      payload["build_exit_code"] = (*summary)["build_exit_code"];
    }
    if (summary->contains("rolled_back")) {
      payload["rolled_back"] = (*summary)["rolled_back"];
    }
    if (!payload["ok"].get<bool>() && summary->contains("error") &&
        (*summary)["error"].is_string()) {
      payload["error"] = (*summary)["error"].get<std::string>();
    }
  }

  if (!payload["ok"].get<bool>() && !payload.contains("error")) {
    if (!stderrText.empty()) {
      payload["error"] = stderrText;
    } else if (!stdoutText.empty()) {
      payload["error"] = stdoutText;
    } else {
      payload["error"] =
          "ultra apply_patch failed with exit code " +
          std::to_string(commandResult.exitCode);
    }
  }
  return formatToolResponse(payload);
}

std::string ToolRouter::execute_run_command(
    const std::map<std::string, std::string>& args) const {
  nlohmann::ordered_json payload;
  payload["stdout"] = "";
  payload["stderr"] = "";
  payload["exit_code"] = 1;
  payload["ok"] = false;
  payload["timed_out"] = false;

  const std::optional<std::string> commandArg = find_argument(args, "command");
  if (!commandArg.has_value()) {
    payload["stderr"] = "missing required argument: command";
    return formatToolResponse(payload);
  }

  const std::filesystem::path cwdPath(
      find_argument(args, "cwd")
          .value_or(std::filesystem::current_path().generic_string()));
  const int timeoutMs = parse_int_argument(args, "timeout_ms", 30000, 1);
  (void)timeoutMs;

  std::error_code ec;
  if (!std::filesystem::exists(cwdPath, ec) || ec ||
      !std::filesystem::is_directory(cwdPath, ec)) {
    payload["stderr"] = "cwd is not a directory: " + cwdPath.generic_string();
    return formatToolResponse(payload);
  }

  // TODO(governance): enforce forbiddenActions/protectedPaths checks before shell execution.
  const std::error_code tempError;
  const std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path stdoutPath =
      (ec ? std::filesystem::path("ultra_tool_stdout.log")
          : tempDir / ("ultra_tool_stdout_" + std::to_string(suffix) + ".log"));
  const std::filesystem::path stderrPath =
      (ec ? std::filesystem::path("ultra_tool_stderr.log")
          : tempDir / ("ultra_tool_stderr_" + std::to_string(suffix) + ".log"));
  (void)tempError;

  const std::string executionCommand =
#if defined(_WIN32)
      "cd /d " + quoteForShell(cwdPath.string()) + " && " + *commandArg;
#else
      "cd " + quoteForShell(cwdPath.string()) + " && " + *commandArg;
#endif
  const std::string redirected = executionCommand +
                                 " > " + quoteForShell(stdoutPath.string()) +
                                 " 2> " + quoteForShell(stderrPath.string());

#if defined(_WIN32)
  ultra::platform::WindowsProcessExecutor executor;
  const ultra::platform::ProcessResult processResult =
      executor.execute(redirected);
#else
  ultra::platform::UnixProcessExecutor executor;
  const ultra::platform::ProcessResult processResult =
      executor.execute(redirected);
#endif

  std::string stdoutText = slurpFile(stdoutPath);
  std::string stderrText = slurpFile(stderrPath);
  stdoutText = limit_output(std::move(stdoutText), kMaxCommandOutputBytes);
  stderrText = limit_output(std::move(stderrText), kMaxCommandOutputBytes);

  std::error_code removeError;
  std::filesystem::remove(stdoutPath, removeError);
  removeError.clear();
  std::filesystem::remove(stderrPath, removeError);

  payload["stdout"] = stdoutText;
  payload["stderr"] = stderrText;
  payload["exit_code"] = processResult.exitCode;
  payload["timed_out"] = false;
  payload["ok"] = processResult.exitCode == 0;
  return formatToolResponse(payload);
}

std::optional<std::string> ToolRouter::find_argument(
    const std::map<std::string, std::string>& args,
    const std::string& key) {
  const auto it = args.find(key);
  if (it == args.end() || it->second.empty()) {
    return std::nullopt;
  }
  return it->second;
}

std::string ToolRouter::quote_argument(const std::string& raw) {
  return quoteForShell(raw);
}

bool ToolRouter::parse_bool_argument(
    const std::map<std::string, std::string>& args,
    const std::string& key,
    const bool defaultValue) {
  const std::optional<std::string> value = find_argument(args, key);
  if (!value.has_value()) {
    return defaultValue;
  }

  const std::string normalized = lowerAscii(*value);
  if (normalized == "1" || normalized == "true" || normalized == "yes") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no") {
    return false;
  }
  return defaultValue;
}

int ToolRouter::parse_int_argument(
    const std::map<std::string, std::string>& args,
    const std::string& key,
    const int defaultValue,
    const int minimumValue) {
  const std::optional<std::string> value = find_argument(args, key);
  if (!value.has_value()) {
    return std::max(defaultValue, minimumValue);
  }
  try {
    return std::max(std::stoi(*value), minimumValue);
  } catch (...) {
    return std::max(defaultValue, minimumValue);
  }
}

std::string ToolRouter::error_json(std::string tool, std::string message) {
  nlohmann::ordered_json payload;
  payload["tool"] = std::move(tool);
  payload["ok"] = false;
  payload["error"] = std::move(message);
  return "ERROR: " + payload.dump();
}

std::string ToolRouter::success_json(const std::string& tool,
                                     const std::string& payload) {
  nlohmann::ordered_json envelope;
  envelope["tool"] = tool;
  envelope["ok"] = true;
  envelope["payload"] = payload;
  return envelope.dump();
}

std::string ToolRouter::limit_output(std::string text,
                                     const std::size_t limitBytes) {
  if (text.size() <= limitBytes) {
    return text;
  }
  constexpr std::string_view kNotice = "\n...[truncated by ToolRouter]";
  const std::size_t keep = limitBytes > kNotice.size()
                               ? limitBytes - kNotice.size()
                               : 0U;
  text.resize(keep);
  text += kNotice;
  return text;
}

bool ToolRouter::is_excluded_directory(const std::filesystem::path& path) {
  static const std::vector<std::string> kExcluded = {
      ".ultra",
      ".ultra_daemon",
      ".git",
      "node_modules",
      "__pycache__",
      "build",
  };

  for (const auto& part : path) {
    const std::string name = lowerAscii(part.generic_string());
    if (std::find(kExcluded.begin(), kExcluded.end(), name) != kExcluded.end()) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> ToolRouter::build_command(
    const std::string& tool,
    const std::map<std::string, std::string>& args) const {
  if (tool == "query_symbol") {
    const auto target = find_argument(args, "target");
    if (!target.has_value()) {
      return std::nullopt;
    }
    return "ultra ai_query " + quote_argument(*target);
  }

  if (tool == "read_source") {
    const auto file = find_argument(args, "file");
    if (!file.has_value()) {
      return std::nullopt;
    }
    return "ultra ai_source " + quote_argument(*file);
  }

  if (tool == "impact_analysis") {
    const auto target = find_argument(args, "target");
    if (!target.has_value()) {
      return std::nullopt;
    }
    return "ultra ai_impact " + quote_argument(*target);
  }

  if (tool == "get_context") {
    const auto query = find_argument(args, "query");
    if (!query.has_value()) {
      return std::nullopt;
    }
    return "ultra ai_context " + quote_argument(*query);
  }

  if (tool == "get_status") {
    return std::string("ultra ai_status --verbose");
  }

  return std::nullopt;
}

std::optional<std::string> ToolRouter::fallback_read_source(
    const std::map<std::string, std::string>& args) const {
  if (!fallbackReader_) {
    return std::nullopt;
  }
  const std::optional<std::string> file = find_argument(args, "file");
  if (!file.has_value()) {
    return std::nullopt;
  }
  return fallbackReader_(*file);
}

}  // namespace ultra::runtime::cognitive::tool_router


