#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "ai/orchestration/MultiModelOrchestrator.h"
#include "core/state_manager.h"
#include "runtime/cognitive/ExecutionKernel.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

ultra::ai::SymbolRecord makeSymbol(const std::uint32_t fileId,
                                   const std::uint32_t localIndex,
                                   const std::string& name,
                                   const std::uint32_t lineNumber) {
  ultra::ai::SymbolRecord symbol;
  symbol.fileId = fileId;
  symbol.symbolId = ultra::ai::SymbolTable::composeSymbolId(fileId, localIndex);
  symbol.name = name;
  symbol.signature = "int " + name + "()";
  symbol.symbolType = ultra::ai::SymbolType::Function;
  symbol.visibility = ultra::ai::Visibility::Public;
  symbol.lineNumber = lineNumber;
  return symbol;
}

ultra::ai::RuntimeState makeExecutionState() {
  ultra::ai::RuntimeState state;

  ultra::ai::FileRecord file;
  file.fileId = 1U;
  file.path = "core.cpp";
  state.files = {file};
  state.symbols = {makeSymbol(1U, 1U, "coreFn", 12U)};

  ultra::ai::SymbolNode symbolNode;
  symbolNode.name = "coreFn";
  symbolNode.definedIn = "core.cpp";
  symbolNode.centrality = 0.5;
  state.symbolIndex["coreFn"] = symbolNode;

  return state;
}

void writeFile(const fs::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output << content;
}

}  // namespace

TEST(ModelExecutionKernel, RoutesRequestsThroughRegistry) {
  const fs::path root = fs::temp_directory_path() / "ultra_test" /
                        "model_execution_kernel";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".ultra", ec);
  ASSERT_FALSE(ec);

  writeFile(
      root / ".ultra" / "models.json",
      R"({
  "providers": {
    "openai": {
      "api_key": "test-key",
      "model": "gpt-4.1-mini",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "stop",
            "message": {
              "content": "deterministic reply",
              "tool_calls": [
                {
                  "function": {
                    "arguments": "{\"path\":\"src/runtime/cognitive/ExecutionKernel.cpp\"}",
                    "name": "lookup_symbol"
                  }
                }
              ]
            }
          }
        ],
        "latency_ms": 5,
        "usage": {
          "completion_tokens": 5,
          "prompt_tokens": 10,
          "total_tokens": 15
        }
      }
    }
  }
})");
  writeFile(
      root / ".ultra" / "model_routing.json",
      R"({
  "routing": {
    "planning": "deepseek",
    "reasoning": "deepseek",
    "coding": "ollama",
    "analysis": "openai"
  }
})");

  ultra::core::StateManager manager(root);
  manager.replaceState(makeExecutionState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(256U);

  auto orchestrator =
      std::make_shared<ultra::ai::orchestration::MultiModelOrchestrator>(root);
  ultra::runtime::ExecutionKernel kernel(manager, orchestrator);

  ultra::runtime::Action action;
  action.type = ultra::runtime::ActionType::ModelGenerate;
  action.snapshotVersion = state.snapshot.version;

  ultra::ai::model::ModelRequest request;
  request.prompt = "Summarize coreFn.";
  request.systemPrompt = "Be deterministic.";
  request.maxTokens = 64U;
  request.temperature = 0.0;
  request.toolsAvailable = {"impact_scan", "lookup_symbol"};
  request.contextPayload = {
      {"path", "src/runtime/cognitive/ExecutionKernel.cpp"},
      {"symbol", "coreFn"},
  };
  action.modelRequest = request;

  ultra::ai::orchestration::OrchestrationContext context;
  context.taskType = ultra::ai::orchestration::TaskType::Analysis;
  context.complexity = ultra::ai::orchestration::TaskComplexity::Low;
  context.priority = ultra::ai::orchestration::TaskPriority::Standard;
  context.latencyBudgetMs = 1000U;
  context.tokenBudget = 64U;
  context.availableModels = {"openai"};
  action.orchestrationContext = context;

  const ultra::runtime::Result result = kernel.execute(action, state);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.message, "Model generation completed.");
  EXPECT_EQ(result.payload["orchestration_context"].value("task_type", std::string{}),
            "analysis");
  EXPECT_EQ(result.payload["response"].value("text_output", std::string{}),
            "deterministic reply");
  EXPECT_EQ(result.payload["response"]["usage_stats"].value("total_tokens", 0U),
            15U);
  ASSERT_EQ(result.payload["response"]["tool_calls"].size(), 1U);
  EXPECT_EQ(
      result.payload["response"]["tool_calls"][0]["arguments"].value(
          "path", std::string{}),
      "src/runtime/cognitive/ExecutionKernel.cpp");
  EXPECT_TRUE(std::find(result.normalizedPaths.begin(),
                        result.normalizedPaths.end(),
                        "src/runtime/cognitive/ExecutionKernel.cpp") !=
              result.normalizedPaths.end());
  EXPECT_EQ(result.risk, ultra::runtime::RiskLevel::Low);

  fs::remove_all(root, ec);
}
