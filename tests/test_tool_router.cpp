#include <gtest/gtest.h>

#include "runtime/cognitive/tool_router/ToolRouter.h"

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

}  // namespace
