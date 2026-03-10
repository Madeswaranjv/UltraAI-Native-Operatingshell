#include <gtest/gtest.h>

#include "ai/model/ModelAdapterRegistry.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

void setEnvVar(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}

void writeFile(const fs::path& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary);
  ASSERT_TRUE(output.is_open());
  output << content;
}

class MockAdapter final : public ultra::ai::model::IModelAdapter {
 public:
  bool initialize(const nlohmann::ordered_json& config,
                  std::string& error) override {
    config_ = config;
    error.clear();
    return true;
  }

  ultra::ai::model::ModelResponse generate(
      const ultra::ai::model::ModelRequest& request) override {
    ultra::ai::model::ModelResponse response;
    response.ok = true;
    response.textOutput = request.prompt;
    return response;
  }

  ultra::ai::model::ModelResponse stream(
      const ultra::ai::model::ModelRequest& request,
      const ultra::ai::model::StreamCallback& onChunk) override {
    ultra::ai::model::ModelResponse response = generate(request);
    if (onChunk && !response.textOutput.empty()) {
      onChunk(response.textOutput);
    }
    return response;
  }

  std::vector<std::string> tokenize(const std::string& text) const override {
    return {text};
  }

  ultra::ai::model::ModelInfo modelInfo() const override {
    ultra::ai::model::ModelInfo info;
    info.providerName = "custom";
    info.modelName = config_.value("model", std::string{"mock"});
    return info;
  }

  void shutdown() override {}

 private:
  nlohmann::ordered_json config_ = nlohmann::ordered_json::object();
};

}  // namespace

TEST(ModelAdapterRegistry, LoadsConfigAndTranslatesOpenAIResponse) {
  const fs::path root = fs::temp_directory_path() / "ultra_test" /
                        "model_adapter_registry";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / ".ultra", ec);
  ASSERT_FALSE(ec);

  setEnvVar("ULTRA_TEST_OPENAI_KEY", "test-openai-key");
  writeFile(
      root / ".ultra" / "models.json",
      R"({
  "providers": {
    "openai": {
      "api_key": "${ULTRA_TEST_OPENAI_KEY}",
      "model": "gpt-4.1-mini",
      "mock_response": {
        "choices": [
          {
            "finish_reason": "tool_calls",
            "message": {
              "content": "tool planned",
              "tool_calls": [
                {
                  "function": {
                    "arguments": "{\"path\":\"src/main.cpp\"}",
                    "name": "lookup_symbol"
                  }
                }
              ]
            }
          }
        ],
        "latency_ms": 9,
        "usage": {
          "completion_tokens": 7,
          "prompt_tokens": 11,
          "total_tokens": 18
        }
      }
    }
  }
})");

  ultra::ai::model::ModelAdapterRegistry registry(root);
  std::string error;
  ASSERT_TRUE(registry.reloadConfiguration(error)) << error;

  const std::vector<std::string> providers = registry.listProviders();
  ASSERT_EQ(providers.size(), 3U);
  EXPECT_EQ(providers[0], "deepseek");
  EXPECT_EQ(providers[1], "ollama");
  EXPECT_EQ(providers[2], "openai");

  const nlohmann::ordered_json config = registry.providerConfiguration("openai");
  ASSERT_TRUE(config.is_object());
  EXPECT_EQ(config.value("api_key", std::string{}), "test-openai-key");

  std::unique_ptr<ultra::ai::model::IModelAdapter> adapter =
      registry.create("openai", error);
  ASSERT_NE(adapter, nullptr) << error;

  ultra::ai::model::ModelRequest request;
  request.prompt = "Find symbol path.";
  request.systemPrompt = "Be deterministic.";
  request.maxTokens = 128U;
  request.temperature = 0.0;

  const ultra::ai::model::ModelResponse response = adapter->generate(request);
  ASSERT_TRUE(response.ok);
  EXPECT_EQ(response.textOutput, "tool planned");
  ASSERT_EQ(response.toolCalls.size(), 1U);
  EXPECT_EQ(response.toolCalls[0].name, "lookup_symbol");
  EXPECT_EQ(response.toolCalls[0].arguments.value("path", std::string{}),
            "src/main.cpp");
  EXPECT_EQ(response.usageStats.promptTokens, 11U);
  EXPECT_EQ(response.usageStats.completionTokens, 7U);
  EXPECT_EQ(response.usageStats.totalTokens, 18U);
  EXPECT_EQ(response.finishReason, "tool_calls");
  EXPECT_EQ(response.latencyMs, 9U);
  EXPECT_EQ(adapter->modelInfo().modelName, "gpt-4.1-mini");

  fs::remove_all(root, ec);
}

TEST(ModelAdapterRegistry, SupportsRuntimeProviderRegistration) {
  ultra::ai::model::ModelAdapterRegistry registry(fs::temp_directory_path());
  registry.registerProvider("custom", []() {
    return std::make_unique<MockAdapter>();
  });

  const std::vector<std::string> providers = registry.listProviders();
  EXPECT_TRUE(std::find(providers.begin(), providers.end(), "custom") !=
              providers.end());

  std::string error;
  std::unique_ptr<ultra::ai::model::IModelAdapter> adapter =
      registry.create("custom", error);
  ASSERT_NE(adapter, nullptr) << error;

  ultra::ai::model::ModelRequest request;
  request.prompt = "echo";
  const ultra::ai::model::ModelResponse response = adapter->generate(request);
  ASSERT_TRUE(response.ok);
  EXPECT_EQ(response.textOutput, "echo");
}

TEST(ModelAdapterRegistry, RejectsUnknownProvider) {
  ultra::ai::model::ModelAdapterRegistry registry(fs::temp_directory_path());

  std::string error;
  std::unique_ptr<ultra::ai::model::IModelAdapter> adapter =
      registry.create("unknown-provider", error);
  EXPECT_EQ(adapter, nullptr);
  EXPECT_NE(error.find("not registered"), std::string::npos);
}
