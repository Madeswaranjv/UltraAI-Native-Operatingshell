#pragma once

#include "ModelRequest.h"
#include "ModelResponse.h"

#include <external/json.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ultra::ai::model {

struct ModelInfo {
  std::string providerName;
  std::string modelName;
  bool supportsStreaming{false};
  bool supportsToolCalls{false};
  bool localProvider{false};
  bool reasoningCapable{false};
  std::size_t contextWindow{0U};
};

using StreamCallback = std::function<void(const std::string& chunk)>;

class IModelAdapter {
 public:
  virtual ~IModelAdapter() = default;

  virtual bool initialize(const nlohmann::ordered_json& config,
                          std::string& error) = 0;
  [[nodiscard]] virtual ModelResponse generate(const ModelRequest& request) = 0;
  [[nodiscard]] virtual ModelResponse stream(const ModelRequest& request,
                                            const StreamCallback& onChunk) = 0;
  [[nodiscard]] virtual std::vector<std::string> tokenize(
      const std::string& text) const = 0;
  [[nodiscard]] virtual ModelInfo modelInfo() const = 0;
  virtual void shutdown() = 0;
};

inline nlohmann::ordered_json toJson(const ModelInfo& info) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["context_window"] = info.contextWindow;
  payload["local_provider"] = info.localProvider;
  payload["model_name"] = info.modelName;
  payload["provider_name"] = info.providerName;
  payload["reasoning_capable"] = info.reasoningCapable;
  payload["supports_streaming"] = info.supportsStreaming;
  payload["supports_tool_calls"] = info.supportsToolCalls;
  return payload;
}

inline nlohmann::ordered_json toJson(const ModelRequest& request) {
  std::vector<std::string> tools = request.toolsAvailable;
  std::sort(tools.begin(), tools.end());
  tools.erase(std::unique(tools.begin(), tools.end()), tools.end());

  nlohmann::ordered_json toolsPayload = nlohmann::ordered_json::array();
  for (const std::string& tool : tools) {
    toolsPayload.push_back(tool);
  }

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["context_payload"] = request.contextPayload;
  payload["max_tokens"] = request.maxTokens;
  payload["prompt"] = request.prompt;
  payload["system_prompt"] = request.systemPrompt;
  payload["temperature"] = request.temperature;
  payload["tools_available"] = std::move(toolsPayload);
  return payload;
}

inline nlohmann::ordered_json toJson(const ToolCall& toolCall) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["arguments"] = toolCall.arguments;
  payload["name"] = toolCall.name;
  return payload;
}

inline nlohmann::ordered_json toJson(const UsageStats& stats) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["completion_tokens"] = stats.completionTokens;
  payload["prompt_tokens"] = stats.promptTokens;
  payload["total_tokens"] = stats.totalTokens;
  return payload;
}

inline nlohmann::ordered_json toJson(const ModelResponse& response) {
  nlohmann::ordered_json toolCalls = nlohmann::ordered_json::array();
  for (const ToolCall& toolCall : response.toolCalls) {
    toolCalls.push_back(toJson(toolCall));
  }

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["error_code"] = toString(response.errorCode);
  payload["error_message"] = response.errorMessage;
  payload["finish_reason"] = response.finishReason;
  payload["latency_ms"] = response.latencyMs;
  payload["ok"] = response.ok;
  payload["text_output"] = response.textOutput;
  payload["tool_calls"] = std::move(toolCalls);
  payload["usage_stats"] = toJson(response.usageStats);
  return payload;
}

}  // namespace ultra::ai::model
