#pragma once

#include "ModelRequest.h"
#include "ModelResponse.h"

#include <external/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
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

inline std::string providerSchemaToolName(std::string tool) {
  return tool;
}

inline std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

inline std::string modelRoleFromRequest(const ModelRequest& request) {
  if (!request.contextPayload.is_object() ||
      !request.contextPayload.contains("model_role") ||
      !request.contextPayload.at("model_role").is_string()) {
    return {};
  }
  return lowerAscii(request.contextPayload.at("model_role").get<std::string>());
}

inline nlohmann::ordered_json configuredModelRoles(
    const nlohmann::ordered_json& config) {
  if (config.is_object()) {
    if (config.contains("models") && config.at("models").is_object()) {
      return config.at("models");
    }
    if (config.contains("roles") && config.at("roles").is_object()) {
      return config.at("roles");
    }
  }
  return nlohmann::ordered_json::object();
}

inline std::string defaultConfiguredModelName(const nlohmann::ordered_json& config,
                                              std::string fallback) {
  if (config.is_object() && config.contains("model") &&
      config.at("model").is_string()) {
    return config.at("model").get<std::string>();
  }

  const nlohmann::ordered_json roles = configuredModelRoles(config);
  if (roles.contains("default") && roles.at("default").is_string()) {
    return roles.at("default").get<std::string>();
  }
  for (auto it = roles.begin(); it != roles.end(); ++it) {
    if (it.value().is_string()) {
      return it.value().get<std::string>();
    }
  }
  return fallback;
}

inline std::string configuredModelNameForRequest(
    const nlohmann::ordered_json& config,
    const ModelRequest& request,
    std::string fallback) {
  const nlohmann::ordered_json roles = configuredModelRoles(config);
  const std::string role = modelRoleFromRequest(request);
  if (!role.empty() && roles.contains(role) && roles.at(role).is_string()) {
    return roles.at(role).get<std::string>();
  }
  return defaultConfiguredModelName(config, std::move(fallback));
}

inline std::vector<std::string> providerSchemaToolNames(
    const ModelRequest& request) {
  std::vector<std::string> tools = request.toolsAvailable;
  std::sort(tools.begin(), tools.end());
  tools.erase(std::unique(tools.begin(), tools.end()), tools.end());
  for (std::string& tool : tools) {
    tool = providerSchemaToolName(std::move(tool));
  }
  std::sort(tools.begin(), tools.end());
  tools.erase(std::unique(tools.begin(), tools.end()), tools.end());
  return tools;
}

inline nlohmann::ordered_json toJson(const ModelRequest& request) {
  std::vector<std::string> tools = providerSchemaToolNames(request);

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
