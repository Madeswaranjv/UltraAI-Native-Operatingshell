#include <gtest/gtest.h>

#include "runtime/cognitive/tool_router/ToolRouter.h"

#include <external/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using ultra::runtime::cognitive::tool_router::ToolRouter;

TEST(ToolRouterTest, ExecutesValidToolMapping) {
  std::vector<std::string> observedCommands;

  ToolRouter router(
      [&observedCommands](const std::string& command) {
        observedCommands.push_back(command);
        return ToolRouter::CommandResult{0, "query-ok"};
      });

  const std::string output =
      router.route_and_execute("query_symbol", {{"target", "ExecutionKernel"}});

  EXPECT_EQ(output, "query-ok");
  ASSERT_EQ(observedCommands.size(), 1U);
  EXPECT_EQ(observedCommands.front(), "ultra ai_query \"ExecutionKernel\"");
}

TEST(ToolRouterTest, RejectsInvalidToolRequests) {
  bool invoked = false;
  ToolRouter router([&invoked](const std::string&) {
    invoked = true;
    return ToolRouter::CommandResult{0, "unexpected"};
  });

  const std::string output = router.route_and_execute("invalid_tool", {});

  EXPECT_FALSE(invoked);
  EXPECT_EQ(output.rfind("ERROR:", 0U), 0U);
}

TEST(ToolRouterTest, RetriesOnceAfterFailure) {
  std::size_t callCount = 0U;
  ToolRouter router([&callCount](const std::string&) {
    ++callCount;
    if (callCount == 1U) {
      return ToolRouter::CommandResult{1, "[ERROR] daemon_unreachable"};
    }
    return ToolRouter::CommandResult{0, "retry-ok"};
  });

  const std::string output =
      router.route_and_execute("get_status", std::map<std::string, std::string>{});

  EXPECT_EQ(callCount, 2U);
  EXPECT_EQ(output, "retry-ok");
}

TEST(ToolRouterTest, FallsBackToFileReadWhenReadSourceFails) {
  std::size_t callCount = 0U;
  ToolRouter router(
      [&callCount](const std::string&) {
        ++callCount;
        return ToolRouter::CommandResult{1, "[ERROR] command_failed"};
      },
      [](const std::string& path) -> std::optional<std::string> {
        if (path == "src/runtime/cognitive/ExecutionKernel.h") {
          return std::string("fallback-content");
        }
        return std::nullopt;
      });

  const std::string output = router.route_and_execute(
      "read_source", {{"file", "src/runtime/cognitive/ExecutionKernel.h"}});

  EXPECT_EQ(callCount, 2U);
  EXPECT_EQ(output, "fallback-content");
}

TEST(ToolRouterTest, ApplyPatchPropagatesFileVerificationFailure) {
  std::string observedCommand;
  ToolRouter router([&observedCommand](const std::string& command) {
    observedCommand = command;

    ToolRouter::CommandResult result;
    result.exitCode = 1;
    result.output =
        std::string("[INFO] [BUILD] Executing: git apply\n") +
        R"json({"applied":false,"build_executed":false,"build_exit_code":1,"build_required":false,"error":"patch_skipped_no_match","file_verified":false,"ok":false,"rolled_back":false})json";
    return result;
  });

  const std::string output = router.route_and_execute(
      "apply_patch",
      {{"project_path", std::filesystem::current_path().generic_string()},
       {"diff",
        "diff --git a/test.txt b/test.txt\n"
        "--- a/test.txt\n"
        "+++ b/test.txt\n"
        "@@ -1 +1 @@\n"
        "-alpha\n"
        "+gamma\n"}});

  ASSERT_EQ(output.rfind("ERROR: ", 0U), 0U);
  const nlohmann::json payload = nlohmann::json::parse(output.substr(7));
  EXPECT_FALSE(payload.at("ok").get<bool>());
  EXPECT_FALSE(payload.at("applied").get<bool>());
  EXPECT_FALSE(payload.at("file_verified").get<bool>());
  EXPECT_EQ(payload.at("error").get<std::string>(), "patch_skipped_no_match");
  ASSERT_TRUE(payload.contains("patch_result"));
  EXPECT_FALSE(payload["patch_result"].at("file_verified").get<bool>());
  EXPECT_EQ(payload["patch_result"].at("error").get<std::string>(),
            "patch_skipped_no_match");
  EXPECT_NE(observedCommand.find("ultra apply_patch"), std::string::npos);
}

TEST(ToolRouterTest, AcceptsHyphenatedApplyPatchAlias) {
  std::string observedCommand;
  ToolRouter router([&observedCommand](const std::string& command) {
    observedCommand = command;
    return ToolRouter::CommandResult{
        0,
        R"json({"applied":true,"file_verified":true,"ok":true})json",
        "",
        0};
  });

  const std::string output = router.route_and_execute(
      "apply-patch",
      {{"project_path", std::filesystem::current_path().generic_string()},
       {"diff",
        "diff --git a/test.txt b/test.txt\n"
        "--- a/test.txt\n"
        "+++ b/test.txt\n"
        "@@ -1 +1 @@\n"
        "-alpha\n"
        "+beta\n"}});

  const nlohmann::json payload = nlohmann::json::parse(output);
  EXPECT_TRUE(payload.at("ok").get<bool>());
  EXPECT_TRUE(payload.at("applied").get<bool>());
  EXPECT_TRUE(payload.at("file_verified").get<bool>());
  EXPECT_NE(observedCommand.find("ultra apply_patch"), std::string::npos);
}

}  // namespace
