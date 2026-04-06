#include <gtest/gtest.h>

#include "ai/SymbolTable.h"
#include "ai/orchestration/MultiModelOrchestrator.h"
#include "core/state_manager.h"
#include "runtime/cognitive/ExecutionKernel.h"
#include "runtime/cognitive/contract_enforcement.h"
#include <external/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>
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


void writeJsonFile(const fs::path& path, const nlohmann::json& payload) {
  writeFile(path, payload.dump(2));
}

std::string readFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream stream;
  stream << input.rdbuf();
  return stream.str();
}

std::string normalizeNewlines(std::string value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (std::size_t index = 0U; index < value.size(); ++index) {
    if (value[index] == '\r') {
      continue;
    }
    normalized.push_back(value[index]);
  }
  return normalized;
}


std::string shellQuote(const fs::path& path) {
  return "\"" + path.string() + "\"";
}

int runCommandInDirectory(const fs::path& root, const std::string& command) {
#if defined(_WIN32)
  const std::string shellCommand = "cd /d " + shellQuote(root) + " && " + command;
#else
  const std::string shellCommand = "cd " + shellQuote(root) + " && " + command;
#endif
  return std::system(shellCommand.c_str());
}

fs::path locateUltraBinary() {
#if defined(_WIN32)
  const std::string executableName = "ultra.exe";
#else
  const std::string executableName = "ultra";
#endif

  for (fs::path cursor = fs::current_path(); !cursor.empty();) {
    const std::vector<fs::path> candidates = {
        cursor / executableName,
        cursor / "Release" / executableName,
        cursor / "build" / executableName,
        cursor / "build" / "Release" / executableName,
    };
    for (const fs::path& candidate : candidates) {
      std::error_code ec;
      if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec) && !ec) {
        return candidate;
      }
    }

    const fs::path parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }

  return {};
}

void writeUltraShim(const fs::path& root, const fs::path& ultraBinary) {
#if defined(_WIN32)
  writeFile(root / "ultra.bat",
            "@echo off\r\n\"" + ultraBinary.string() + "\" %*\r\n");
#else
  const fs::path shimPath = root / "ultra";
  writeFile(shimPath,
            "#!/usr/bin/env sh\n\"" + ultraBinary.string() + "\" \"$@\"\n");
  std::error_code ec;
  fs::permissions(shimPath,
                  fs::perms::owner_exec | fs::perms::owner_read |
                      fs::perms::owner_write,
                  fs::perm_options::add,
                  ec);
  ASSERT_FALSE(ec);
#endif
}

std::string currentPathValue() {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t length = 0U;
  if (_dupenv_s(&value, &length, "PATH") != 0 || value == nullptr) {
    return {};
  }
  const std::string pathValue(value);
  std::free(value);
  return pathValue;
#else
  const char* value = std::getenv("PATH");
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

class ScopedPathOverride {
 public:
  explicit ScopedPathOverride(std::string prefix)
      : original_(currentPathValue()) {
#if defined(_WIN32)
    _putenv_s("PATH", (std::move(prefix) + ";" + original_).c_str());
#else
    setenv("PATH", (std::move(prefix) + ":" + original_).c_str(), 1);
#endif
  }

  ~ScopedPathOverride() {
#if defined(_WIN32)
    _putenv_s("PATH", original_.c_str());
#else
    setenv("PATH", original_.c_str(), 1);
#endif
  }

 private:
  std::string original_;
};
}  // namespace

TEST(ModelExecutionKernel, RoutesRequestsThroughRegistry) {
  const fs::path root = fs::temp_directory_path() / "ultra_test" /
                        "model_execution_kernel";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".ultra", ec);
  fs::create_directories(root / "src" / "runtime" / "cognitive", ec);
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
                    "arguments": "{\"path\":\".\"}",
                    "name": "list_dir"
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
      root / "src" / "runtime" / "cognitive" / "ExecutionKernel.cpp",
      "int coreFn() { return 42; }\n");
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
  action.id = "task.model_generate";
  action.type = ultra::runtime::ActionType::ModelGenerate;
  action.snapshotVersion = state.snapshot.version;

  ultra::ai::model::ModelRequest request;
  request.prompt = "Summarize coreFn.";
  request.systemPrompt = "Be deterministic.";
  request.maxTokens = 64U;
  request.temperature = 0.0;
  request.toolsAvailable = {"list_dir"};
  request.contextPayload = {
      {"path", "."},
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

  const ultra::runtime::contracts::ScopedTaskGraphAuthorization authorization(
      action.id);
  const ultra::runtime::Result result = kernel.execute(action, state);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.message, "Model generation completed and tool calls executed.");
  EXPECT_EQ(result.payload["orchestration_context"].value("task_type", std::string{}),
            "analysis");
  EXPECT_EQ(result.payload["response"].value("text_output", std::string{}),
            "deterministic reply");
  EXPECT_EQ(result.payload.value("selected_provider", std::string{}),
            "openai");
  EXPECT_EQ(result.payload["response"]["usage_stats"].value("total_tokens", 0U),
            15U);
  ASSERT_EQ(result.payload["response"]["tool_calls"].size(), 1U);
  EXPECT_EQ(result.payload["response"]["tool_calls"][0].value("name", std::string{}),
            "list_dir");
  EXPECT_EQ(
      result.payload["response"]["tool_calls"][0]["arguments"].value(
          "path", std::string{}),
      ".");
  ASSERT_EQ(result.payload["tool_results"].size(), 1U);
  EXPECT_EQ(result.payload["tool_results"][0].value("tool", std::string{}),
            "list_dir");
  EXPECT_EQ(result.payload.value("tool_execution_summary", std::string{}),
            "Executed tool calls: list_dir.");
  EXPECT_EQ(result.risk, ultra::runtime::RiskLevel::Low);

  fs::remove_all(root, ec);
}

TEST(ModelExecutionKernel, InfersTextToolCallsAndExecutesApplyPatch) {
  const fs::path root = fs::temp_directory_path() / "ultra_test" /
                        "model_execution_kernel_text_tool";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".ultra", ec);
  ASSERT_FALSE(ec);

  writeFile(root / "test.txt", "alpha\n");
  ASSERT_EQ(runCommandInDirectory(root, "git init -q"), 0);

  const std::string diff =
      "diff --git a/test.txt b/test.txt\n"
      "--- a/test.txt\n"
      "+++ b/test.txt\n"
      "@@ -1 +1 @@\n"
      "-alpha\n"
      "+gamma\n";
  const std::string toolPayload =
      nlohmann::json{{"tool", "apply_patch"}, {"changes", diff}}.dump();

  nlohmann::json modelConfig = nlohmann::json::object();
  modelConfig["providers"]["openai"]["api_key"] = "test-key";
  modelConfig["providers"]["openai"]["model"] = "gpt-4.1-mini";
  modelConfig["providers"]["openai"]["mock_response"]["choices"] =
      nlohmann::json::array({
          nlohmann::json{{"finish_reason", "stop"},
                         {"message", {{"content", toolPayload}}}},
      });
  modelConfig["providers"]["openai"]["mock_response"]["latency_ms"] = 5;
  modelConfig["providers"]["openai"]["mock_response"]["usage"] =
      nlohmann::json{{"completion_tokens", 5},
                     {"prompt_tokens", 10},
                     {"total_tokens", 15}};
  writeJsonFile(root / ".ultra" / "models.json", modelConfig);
  writeJsonFile(
      root / ".ultra" / "model_routing.json",
      nlohmann::json{{"routing",
                     {{"planning", "deepseek"},
                      {"reasoning", "deepseek"},
                      {"coding", "ollama"},
                      {"analysis", "openai"}}}});

  const fs::path ultraBinary = locateUltraBinary();
  ASSERT_FALSE(ultraBinary.empty());
  writeUltraShim(root, ultraBinary);
  ScopedPathOverride pathGuard(root.string());

  ultra::core::StateManager manager(root);
  manager.replaceState(makeExecutionState());
  const ultra::runtime::CognitiveState state = manager.createCognitiveState(256U);

  auto orchestrator =
      std::make_shared<ultra::ai::orchestration::MultiModelOrchestrator>(root);
  ultra::runtime::ExecutionKernel kernel(manager, orchestrator);

  ultra::runtime::Action action;
  action.id = "task.model_generate.text_tool";
  action.type = ultra::runtime::ActionType::ModelGenerate;
  action.snapshotVersion = state.snapshot.version;

  ultra::ai::model::ModelRequest request;
  request.prompt = "Apply the requested patch.";
  request.systemPrompt = "Return only a deterministic tool call.";
  request.maxTokens = 128U;
  request.temperature = 0.0;
  request.toolsAvailable = {"apply_patch"};
  request.contextPayload = {
      {"path", "test.txt"},
  };
  action.modelRequest = request;

  ultra::ai::orchestration::OrchestrationContext context;
  context.taskType = ultra::ai::orchestration::TaskType::Analysis;
  context.complexity = ultra::ai::orchestration::TaskComplexity::Low;
  context.priority = ultra::ai::orchestration::TaskPriority::Standard;
  context.latencyBudgetMs = 1000U;
  context.tokenBudget = 128U;
  context.availableModels = {"openai"};
  action.orchestrationContext = context;

  const ultra::runtime::contracts::ScopedTaskGraphAuthorization authorization(
      action.id);
  const ultra::runtime::Result result = kernel.execute(action, state);
  ASSERT_TRUE(result.ok);
  EXPECT_EQ(result.message, "Model generation completed and tool calls executed.");
  EXPECT_TRUE(result.payload.value("tool_calls_inferred", false));
  EXPECT_TRUE(result.payload.value("tool_call_detected", false));
  EXPECT_TRUE(result.payload.value("tool_router_executed", false));
  EXPECT_EQ(result.payload.value("tool_router_transport", std::string{}),
            "tool_executor");
  ASSERT_TRUE(result.payload.contains("tool_execution"));
  EXPECT_EQ(result.payload["tool_execution"].value("tool", std::string{}),
            "apply_patch");
  EXPECT_TRUE(result.payload["tool_execution"].value("applied", false));
  EXPECT_TRUE(result.payload["tool_execution"].value("file_verified", false));
  EXPECT_TRUE(result.payload["tool_execution"].value("ok", false));
  EXPECT_EQ(normalizeNewlines(readFile(root / "test.txt")), "gamma\n");

  fs::remove_all(root, ec);
}
