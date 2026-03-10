#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "engine/context/ContextBuilder.h"

#include <external/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

ultra::ai::SymbolRecord makeSymbol(const std::uint32_t fileId,
                                   const std::uint32_t localIndex,
                                   const std::string& name,
                                   const std::string& signature,
                                   const ultra::ai::SymbolType symbolType,
                                   const std::uint32_t lineNumber) {
  ultra::ai::SymbolRecord symbol;
  symbol.fileId = fileId;
  symbol.symbolId = ultra::ai::SymbolTable::composeSymbolId(fileId, localIndex);
  symbol.name = name;
  symbol.signature = signature;
  symbol.symbolType = symbolType;
  symbol.visibility = ultra::ai::Visibility::Public;
  symbol.lineNumber = lineNumber;
  return symbol;
}

ultra::ai::RuntimeState makeContextState(const bool shuffled) {
  ultra::ai::RuntimeState state;

  ultra::ai::FileRecord core;
  core.fileId = 1U;
  core.path = "core.cpp";
  ultra::ai::FileRecord service;
  service.fileId = 2U;
  service.path = "service.cpp";
  ultra::ai::FileRecord app;
  app.fileId = 3U;
  app.path = "app.cpp";
  ultra::ai::FileRecord worker;
  worker.fileId = 4U;
  worker.path = "worker.cpp";
  state.files = {core, service, app, worker};

  state.symbols = {
      makeSymbol(1U, 1U, "coreFn", "int coreFn()",
                 ultra::ai::SymbolType::Function, 10U),
      makeSymbol(2U, 1U, "serviceFn", "int serviceFn()",
                 ultra::ai::SymbolType::Function, 20U),
      makeSymbol(3U, 1U, "appMain", "int appMain()",
                 ultra::ai::SymbolType::Function, 30U),
      makeSymbol(4U, 1U, "workerTask", "int workerTask()",
                 ultra::ai::SymbolType::Function, 40U),
      makeSymbol(2U, 2U, "coreFn", "coreFn()", ultra::ai::SymbolType::Import, 21U),
      makeSymbol(3U, 2U, "serviceFn", "serviceFn()",
                 ultra::ai::SymbolType::Import, 31U),
      makeSymbol(4U, 2U, "serviceFn", "serviceFn()",
                 ultra::ai::SymbolType::Import, 41U),
  };

  state.deps.fileEdges = {
      {3U, 2U},
      {2U, 1U},
      {4U, 2U},
      {3U, 2U},
  };
  state.deps.symbolEdges = {
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(3U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(4U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(2U, 1U)},
      {ultra::ai::SymbolTable::composeSymbolId(2U, 1U),
       ultra::ai::SymbolTable::composeSymbolId(1U, 1U)},
  };

  if (shuffled) {
    std::reverse(state.files.begin(), state.files.end());
    std::reverse(state.symbols.begin(), state.symbols.end());
    std::reverse(state.deps.fileEdges.begin(), state.deps.fileEdges.end());
    std::reverse(state.deps.symbolEdges.begin(), state.deps.symbolEdges.end());
  }

  return state;
}

const nlohmann::json* findObjectByStringField(const nlohmann::json& items,
                                              const char* fieldName,
                                              const std::string& expectedValue) {
  if (!items.is_array()) {
    return nullptr;
  }
  for (const auto& item : items) {
    if (!item.is_object()) {
      continue;
    }
    if (item.value(fieldName, std::string{}) == expectedValue) {
      return &item;
    }
  }
  return nullptr;
}

std::vector<std::string> extractStringFieldList(const nlohmann::json& items,
                                                const char* fieldName) {
  std::vector<std::string> values;
  if (!items.is_array()) {
    return values;
  }
  for (const auto& item : items) {
    if (!item.is_object()) {
      continue;
    }
    const std::string value = item.value(fieldName, std::string{});
    if (!value.empty()) {
      values.push_back(value);
    }
  }
  return values;
}

}  // namespace

TEST(ContextBuilderTest, BuildSymbolContextReturnsDefinitionAndReferences) {
  ultra::engine::context::ContextBuilder builder(makeContextState(false));

  const auto slice = builder.buildSymbolContext("coreFn", 1024U);
  const nlohmann::json payload = nlohmann::json::parse(slice.json);

  EXPECT_EQ(payload.value("kind", ""), "symbol_context");
  const nlohmann::json* node =
      findObjectByStringField(payload["nodes"], "name", "coreFn");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node->value("defined_in", ""), "core.cpp");
  EXPECT_EQ((*node)["references"], nlohmann::json::array({"service.cpp"}));

  ASSERT_TRUE(node->contains("definitions"));
  ASSERT_TRUE((*node)["definitions"].is_array());
  ASSERT_EQ((*node)["definitions"].size(), 1U);
  EXPECT_EQ((*node)["definitions"][0].value("file_path", ""), "core.cpp");
  EXPECT_EQ((*node)["definitions"][0].value("line_number", 0U), 10U);

  EXPECT_EQ(payload["impact_region"],
            nlohmann::json::array(
                {"app.cpp", "core.cpp", "service.cpp", "worker.cpp"}));
}

TEST(ContextBuilderTest, BuildFileContextReturnsRelevantSymbols) {
  ultra::engine::context::ContextBuilder builder(makeContextState(false));

  const auto slice = builder.buildFileContext("app.cpp", 1024U);
  const nlohmann::json payload = nlohmann::json::parse(slice.json);

  EXPECT_EQ(payload.value("kind", ""), "file_context");
  const nlohmann::json* file =
      findObjectByStringField(payload["files"], "path", "app.cpp");
  ASSERT_NE(file, nullptr);
  EXPECT_EQ((*file)["dependencies"], nlohmann::json::array({"service.cpp"}));
  EXPECT_EQ((*file)["relevant_symbols"],
            nlohmann::json::array({"appMain", "serviceFn"}));

  const std::vector<std::string> nodeNames =
      extractStringFieldList(payload["nodes"], "name");
  EXPECT_EQ(nodeNames,
            std::vector<std::string>({"serviceFn", "appMain"}));
}

TEST(ContextBuilderTest, ImpactContextIncludesTransitiveDependencies) {
  ultra::engine::context::ContextBuilder builder(makeContextState(false));

  const auto slice = builder.buildImpactContext("coreFn", 1024U, {}, 2U);
  const nlohmann::json payload = nlohmann::json::parse(slice.json);

  EXPECT_EQ(payload.value("kind", ""), "impact_context");
  EXPECT_EQ(payload["impact_region"],
            nlohmann::json::array(
                {"app.cpp", "core.cpp", "service.cpp", "worker.cpp"}));

  const std::vector<std::string> nodeNames =
      extractStringFieldList(payload["nodes"], "name");
  EXPECT_EQ(nodeNames,
            std::vector<std::string>(
                {"coreFn", "serviceFn", "appMain", "workerTask"}));
}

TEST(ContextBuilderTest, ContextRespectsTokenBudget) {
  ultra::engine::context::ContextBuilder builder(makeContextState(false));

  const auto relaxed = builder.buildImpactContext("coreFn", 1024U, {}, 2U);
  const auto tight = builder.buildImpactContext("coreFn", 220U, {}, 2U);
  const nlohmann::json relaxedPayload = nlohmann::json::parse(relaxed.json);
  const nlohmann::json tightPayload = nlohmann::json::parse(tight.json);

  EXPECT_LE(tight.estimatedTokens, 220U);
  EXPECT_LE(tightPayload["metadata"].value("selectedNodeCount", 0U),
            relaxedPayload["metadata"].value("selectedNodeCount", 0U));
  EXPECT_LE(tightPayload["metadata"].value("selectedFileCount", 0U),
            relaxedPayload["metadata"].value("selectedFileCount", 0U));
  EXPECT_TRUE(tightPayload["metadata"].value("truncated", false));
}

TEST(ContextBuilderTest, ContextRankingProducesDeterministicOrdering) {
  ultra::engine::context::ContextBuilder builderA(makeContextState(false));
  ultra::engine::context::ContextBuilder builderB(makeContextState(true));

  const auto sliceA = builderA.buildImpactContext("coreFn", 1024U, {}, 2U);
  const auto sliceB = builderB.buildImpactContext("coreFn", 1024U, {}, 2U);
  const nlohmann::json payload = nlohmann::json::parse(sliceA.json);

  EXPECT_EQ(sliceA.json, sliceB.json);

  const std::vector<std::string> filePaths =
      extractStringFieldList(payload["files"], "path");
  EXPECT_TRUE(std::is_sorted(filePaths.begin(), filePaths.end()));

  std::vector<std::uint64_t> nodeIds;
  for (const auto& node : payload["nodes"]) {
    nodeIds.push_back(node.value("id", 0ULL));
  }
  EXPECT_TRUE(std::is_sorted(nodeIds.begin(), nodeIds.end()));
}
