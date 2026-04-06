#include <gtest/gtest.h>

#include "runtime/cognitive/tools/ToolDefinition.h"
#include "runtime/cognitive/tools/ToolRegistry.h"

#include <string>

namespace {

using ultra::runtime::cognitive::tools::ToolDefinition;
using ultra::runtime::cognitive::tools::ToolRegistry;

TEST(ToolRegistryTest, RegistersDefaultTools) {
  ToolRegistry registry;

  const ToolDefinition* querySymbol = registry.get_tool("query_symbol");
  ASSERT_NE(querySymbol, nullptr);
  ASSERT_EQ(querySymbol->input_params.size(), 1U);
  EXPECT_EQ(querySymbol->input_params.front(), "target");

  EXPECT_NE(registry.get_tool("read_source"), nullptr);
  EXPECT_NE(registry.get_tool("impact_analysis"), nullptr);
  EXPECT_NE(registry.get_tool("get_context"), nullptr);
  EXPECT_NE(registry.get_tool("get_status"), nullptr);
  EXPECT_NE(registry.get_tool("apply_patch"), nullptr);
}

TEST(ToolRegistryTest, RejectsInvalidToolDefinitions) {
  ToolRegistry registry;

  ToolDefinition invalid;
  invalid.name.clear();
  invalid.description = "invalid";
  invalid.output_description = "none";
  registry.register_tool(invalid);

  EXPECT_EQ(registry.get_tool(""), nullptr);
}

TEST(ToolRegistryTest, RegistersCustomTool) {
  ToolRegistry registry;

  ToolDefinition custom;
  custom.name = "custom_probe";
  custom.description = "Custom test tool.";
  custom.input_params = {"arg"};
  custom.output_description = "Probe output.";
  registry.register_tool(custom);

  const ToolDefinition* resolved = registry.get_tool("custom_probe");
  ASSERT_NE(resolved, nullptr);
  EXPECT_EQ(resolved->name, "custom_probe");
  ASSERT_EQ(resolved->input_params.size(), 1U);
  EXPECT_EQ(resolved->input_params.front(), "arg");
}

}  // namespace
