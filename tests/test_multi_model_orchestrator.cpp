#include <gtest/gtest.h>

#include "ai/orchestration/MultiModelOrchestrator.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output << content;
}

}  // namespace

TEST(MultiModelOrchestrator, RoutesPlanningToConfiguredReasoningProvider) {
  const fs::path root = fs::temp_directory_path() / "ultra_test" /
                        "multi_model_orchestrator_routing";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".ultra", ec);
  ASSERT_FALSE(ec);

  writeFile(
      root / ".ultra" / "models.json",
      R"({
  "providers": {
    "deepseek": {
      "api_key": "test-key",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "stop",
            "message": {
              "content": "deep reasoning response"
            }
          }
        ],
        "latency_ms": 11,
        "usage": {
          "completion_tokens": 6,
          "prompt_tokens": 12,
          "total_tokens": 18
        }
      }
    },
    "ollama": {
      "endpoint": "http://localhost:11434",
      "mock_response": {
        "response": "local coding response",
        "latency_ms": 4
      }
    },
    "openai": {
      "api_key": "test-key",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "stop",
            "message": {
              "content": "lightweight analysis response"
            }
          }
        ],
        "latency_ms": 7
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

  ultra::ai::orchestration::MultiModelOrchestrator orchestrator(root);
  ultra::ai::model::ModelRequest request;
  request.prompt = "Plan a change.";

  ultra::ai::orchestration::OrchestrationContext context;
  context.taskType = ultra::ai::orchestration::TaskType::Planning;
  context.complexity = ultra::ai::orchestration::TaskComplexity::High;
  context.priority = ultra::ai::orchestration::TaskPriority::Standard;
  context.tokenBudget = 512U;

  const ultra::ai::model::ModelResponse response =
      orchestrator.generate(request, context);
  ASSERT_TRUE(response.ok);
  EXPECT_EQ(response.textOutput, "deep reasoning response");
  EXPECT_EQ(orchestrator.lastDecision().selectedProvider, "deepseek");
  EXPECT_FALSE(orchestrator.lastDecision().fallbackUsed);

  fs::remove_all(root, ec);
}

TEST(MultiModelOrchestrator, FallsBackToLocalProviderDeterministically) {
  const fs::path root = fs::temp_directory_path() / "ultra_test" /
                        "multi_model_orchestrator_fallback";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".ultra", ec);
  ASSERT_FALSE(ec);

  writeFile(
      root / ".ultra" / "models.json",
      R"({
  "providers": {
    "deepseek": {
      "api_key": "test-key",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "stop",
            "message": {
              "content": "broken"
            }
          }
        ],
        "latency_ms": 3,
        "usage": {
          "completion_tokens": 1,
          "prompt_tokens": 1,
          "total_tokens": 2
        }
      }
    },
    "ollama": {
      "endpoint": "http://localhost:11434",
      "mock_response": {
        "response": "local fallback response",
        "latency_ms": 6
      }
    },
    "openai": {
      "api_key": "test-key",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "stop",
            "message": {
              "content": "cloud fallback response"
            }
          }
        ],
        "latency_ms": 10
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

  ultra::ai::orchestration::MultiModelOrchestrator orchestrator(root);
  ultra::ai::model::ModelRequest request;
  request.prompt = "Reason about this diff.";

  ultra::ai::orchestration::OrchestrationContext context;
  context.taskType = ultra::ai::orchestration::TaskType::Planning;
  context.availableModels = {"deepseek", "ollama", "openai"};

  std::string error;
  ASSERT_TRUE(orchestrator.reloadConfiguration(error)) << error;

  // Force preferred provider failure by replacing the registry-visible config
  // with an invalid DeepSeek mock response.
  writeFile(
      root / ".ultra" / "models.json",
      R"({
  "providers": {
    "deepseek": {
      "api_key": "test-key",
      "mock_response": {
        "choices": []
      }
    },
    "ollama": {
      "endpoint": "http://localhost:11434",
      "mock_response": {
        "response": "local fallback response",
        "latency_ms": 6
      }
    },
    "openai": {
      "api_key": "test-key",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "stop",
            "message": {
              "content": "cloud fallback response"
            }
          }
        ],
        "latency_ms": 10
      }
    }
  }
})");

  ultra::ai::orchestration::MultiModelOrchestrator fallbackOrchestrator(root);
  const ultra::ai::model::ModelResponse response =
      fallbackOrchestrator.generate(request, context);
  ASSERT_TRUE(response.ok);
  EXPECT_EQ(response.textOutput, "local fallback response");
  EXPECT_EQ(fallbackOrchestrator.lastDecision().selectedProvider, "ollama");
  EXPECT_TRUE(fallbackOrchestrator.lastDecision().fallbackUsed);
  ASSERT_GE(fallbackOrchestrator.lastDecision().attemptedProviders.size(), 2U);
  EXPECT_EQ(fallbackOrchestrator.lastDecision().attemptedProviders[0], "deepseek");
  EXPECT_EQ(fallbackOrchestrator.lastDecision().attemptedProviders[1], "ollama");

  fs::remove_all(root, ec);
}

TEST(MultiModelOrchestrator, RespectsAvailableModelConstraints) {
  const fs::path root = fs::temp_directory_path() / "ultra_test" /
                        "multi_model_orchestrator_constraints";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".ultra", ec);
  ASSERT_FALSE(ec);

  writeFile(
      root / ".ultra" / "models.json",
      R"({
  "providers": {
    "deepseek": {
      "api_key": "test-key",
      "mock_response": {
        "choices": []
      }
    },
    "openai": {
      "api_key": "test-key",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "stop",
            "message": {
              "content": "analysis-only response"
            }
          }
        ],
        "latency_ms": 8
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

  ultra::ai::orchestration::MultiModelOrchestrator orchestrator(root);
  ultra::ai::model::ModelRequest request;
  request.prompt = "Analyze this file.";

  ultra::ai::orchestration::OrchestrationContext context;
  context.taskType = ultra::ai::orchestration::TaskType::Planning;
  context.availableModels = {"openai"};

  const ultra::ai::model::ModelResponse response =
      orchestrator.generate(request, context);
  ASSERT_TRUE(response.ok);
  EXPECT_EQ(response.textOutput, "analysis-only response");
  EXPECT_EQ(orchestrator.lastDecision().selectedProvider, "openai");
  ASSERT_EQ(orchestrator.lastDecision().attemptedProviders.size(), 1U);
  EXPECT_EQ(orchestrator.lastDecision().attemptedProviders[0], "openai");

  fs::remove_all(root, ec);
}
