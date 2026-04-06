#include <gtest/gtest.h>

#include "ai/model/IModelAdapter.h"

#include <vector>

namespace {

using ultra::ai::model::ModelRequest;

TEST(ModelAdapterToolsTest, PreservesCanonicalApplyPatchInProviderSchema) {
  ModelRequest request;
  request.toolsAvailable = {"query_symbol", "apply_patch", "apply_patch"};

  const std::vector<std::string> tools =
      ultra::ai::model::providerSchemaToolNames(request);

  ASSERT_EQ(tools.size(), 2U);
  EXPECT_EQ(tools[0], "apply_patch");
  EXPECT_EQ(tools[1], "query_symbol");
}

TEST(ModelAdapterToolsTest, LeavesOtherToolNamesUnchanged) {
  EXPECT_EQ(ultra::ai::model::providerSchemaToolName("read_file"), "read_file");
  EXPECT_EQ(ultra::ai::model::providerSchemaToolName("run_command"),
            "run_command");
}

}  // namespace
