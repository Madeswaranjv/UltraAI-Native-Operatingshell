#include "AnthropicAdapter.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include "../http/HttpTransport.h"

namespace ultra::ai::model::providers {

namespace {

ModelResponse makeError(ModelErrorCode code,
                        std::string message,
                        const std::uint64_t latencyMs = 0U) {
  ModelResponse response;
  response.ok = false;
  response.errorCode = code;
  response.errorMessage = std::move(message);
  response.latencyMs = latencyMs;
  return response;
}

std::vector<std::string> tokenizeWhitespace(const std::string& text) {
  std::istringstream input(text);
  std::vector<std::string> tokens;
  std::string token;
  while (input >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

bool parseArguments(const nlohmann::ordered_json& value,
                    nlohmann::ordered_json& arguments) {
  if (value.is_null()) {
    arguments = nlohmann::ordered_json::object();
    return true;
  }
  if (value.is_object() || value.is_array()) {
    arguments = value;
    return true;
  }
  if (!value.is_string()) {
    return false;
  }
  const std::string text = value.get<std::string>();
  if (text.empty()) {
    arguments = nlohmann::ordered_json::object();
    return true;
  }
  try {
    arguments = nlohmann::ordered_json::parse(text);
    return true;
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}

// Anthropic uses content blocks: each block has a "type".
// tool_use blocks carry id/name/input. text blocks carry text.
bool parseContentBlocks(const nlohmann::ordered_json& content,
                        std::string& textOutput,
                        std::vector<ToolCall>& toolCalls) {
  if (!content.is_array()) {
    return false;
  }

  toolCalls.clear();
  textOutput.clear();

  for (const auto& block : content) {
    if (!block.is_object()) {
      return false;
    }
    const std::string blockType = block.value("type", std::string{});

    if (blockType == "text") {
      textOutput += block.value("text", std::string{});
    } else if (blockType == "thinking") {
      // Extended thinking block — prepend to textOutput
      const std::string thinking = block.value("thinking", std::string{});
      if (!thinking.empty()) {
        textOutput = thinking + "\n\n" + textOutput;
      }
    } else if (blockType == "tool_use") {
      if (!block.contains("name") || !block.at("name").is_string()) {
        return false;
      }
      ToolCall toolCall;
      toolCall.name = block.at("name").get<std::string>();
      const nlohmann::ordered_json inputValue =
          block.contains("input") ? block.at("input")
                                  : nlohmann::ordered_json::object();
      if (!parseArguments(inputValue, toolCall.arguments)) {
        return false;
      }
      toolCalls.push_back(std::move(toolCall));
    }
  }

  std::sort(toolCalls.begin(), toolCalls.end(),
            [](const ToolCall& left, const ToolCall& right) {
              if (left.name != right.name) {
                return left.name < right.name;
              }
              return left.arguments.dump() < right.arguments.dump();
            });
  return true;
}

std::uint64_t readLatencyMs(const nlohmann::ordered_json& config,
                            const nlohmann::ordered_json& response) {
  if (response.contains("latency_ms") && response.at("latency_ms").is_number()) {
    return response.at("latency_ms").get<std::uint64_t>();
  }
  if (config.contains("mock_latency_ms") &&
      config.at("mock_latency_ms").is_number()) {
    return config.at("mock_latency_ms").get<std::uint64_t>();
  }
  return 0U;
}

}  // namespace

bool AnthropicAdapter::initialize(const nlohmann::ordered_json& config,
                                  std::string& error) {
  if (!config.is_null() && !config.is_object()) {
    error = "Anthropic adapter configuration must be an object.";
    initialized_ = false;
    return false;
  }

  config_ = config.is_object() ? config : nlohmann::ordered_json::object();
  info_.providerName = "anthropic";
  info_.modelName =
      config_.value("model", std::string{"claude-sonnet-4-5"});
  info_.supportsStreaming = true;
  info_.supportsToolCalls = true;
  info_.localProvider = false;
  info_.reasoningCapable =
      config_.value("reasoning_capable", false);
  info_.contextWindow =
      config_.value("context_window", std::size_t{200000U});

  const bool hasApiKey =
      config_.contains("api_key") && config_.at("api_key").is_string() &&
      !config_.at("api_key").get<std::string>().empty();
  const bool hasMockResponse = config_.contains("mock_response");
  if (!hasApiKey && !hasMockResponse) {
    error = "Anthropic adapter requires 'api_key' or 'mock_response'.";
    initialized_ = false;
    return false;
  }

  initialized_ = true;
  error.clear();
  return true;
}

ModelResponse AnthropicAdapter::generate(const ModelRequest& request) {
  if (!initialized_) {
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Anthropic adapter is not initialized.");
  }

  if (config_.contains("mock_response")) {
    return translateResponse(config_.at("mock_response"));
  }

  const std::string apiKey =
      config_.value("api_key", std::string{});

  const std::string baseUrl =
      config_.value("base_url", std::string{"https://api.anthropic.com"});
  const std::string url = baseUrl + "/v1/messages";

  const nlohmann::ordered_json payload = buildProviderRequest(request);
  const std::string body = payload.dump();

  const std::unordered_map<std::string, std::string> headers = {
      {"x-api-key",         apiKey},
      {"anthropic-version", "2023-06-01"},
  };

  const http::HttpResponse httpResp = http::httpPost(url, body, headers);

  if (!httpResp.ok) {
    if (httpResp.statusCode == 0) {
      return makeError(ModelErrorCode::ProviderUnavailable,
                       "Anthropic HTTP transport error: " + httpResp.errorMessage);
    }
    try {
      const nlohmann::ordered_json errJson =
          nlohmann::ordered_json::parse(httpResp.body);
      if (errJson.contains("error") && errJson.at("error").is_object()) {
        return makeError(ModelErrorCode::ProviderUnavailable,
                         "Anthropic error: " +
                         errJson.at("error").value("message", std::string{"unknown"}));
      }
    } catch (...) {}
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Anthropic returned HTTP " + std::to_string(httpResp.statusCode));
  }

  try {
    const nlohmann::ordered_json responseJson =
        nlohmann::ordered_json::parse(httpResp.body);
    return translateResponse(responseJson);
  } catch (const nlohmann::json::exception& ex) {
    return makeError(ModelErrorCode::InvalidResponse,
                     std::string("Anthropic response JSON parse failed: ") + ex.what());
  }
}

ModelResponse AnthropicAdapter::stream(const ModelRequest& request,
                                       const StreamCallback& onChunk) {
  ModelResponse response = generate(request);
  if (response.ok && onChunk && !response.textOutput.empty()) {
    onChunk(response.textOutput);
  }
  return response;
}

std::vector<std::string> AnthropicAdapter::tokenize(
    const std::string& text) const {
  return tokenizeWhitespace(text);
}

ModelInfo AnthropicAdapter::modelInfo() const {
  return info_;
}

void AnthropicAdapter::shutdown() {
  initialized_ = false;
}

nlohmann::ordered_json AnthropicAdapter::buildProviderRequest(
    const ModelRequest& request) const {
  std::vector<std::string> tools = providerSchemaToolNames(request);

  // Anthropic uses a top-level "system" field, not a system message in the
  // messages array.
  nlohmann::ordered_json messages = nlohmann::ordered_json::array();
  messages.push_back({{"content", request.prompt}, {"role", "user"}});

  // Tool definitions use input_schema (JSON Schema) rather than parameters.
  nlohmann::ordered_json toolPayload = nlohmann::ordered_json::array();
  for (const std::string& tool : tools) {
    nlohmann::ordered_json item = nlohmann::ordered_json::object();
    item["description"] = tool;
    item["input_schema"] = {{"type", "object"}};
    item["name"] = tool;
    toolPayload.push_back(std::move(item));
  }

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["context_payload"] = request.contextPayload;
  payload["max_tokens"] = request.maxTokens > 0U ? request.maxTokens : 8192U;
  payload["messages"] = std::move(messages);
  payload["model"] = info_.modelName;
  payload["temperature"] = request.temperature;
  payload["tools"] = std::move(toolPayload);
  if (!request.systemPrompt.empty()) {
    payload["system"] = request.systemPrompt;
  }
  return payload;
}

ModelResponse AnthropicAdapter::translateResponse(
    const nlohmann::ordered_json& providerResponse) const {
  // Anthropic response shape:
  // { "id": "...", "type": "message", "role": "assistant",
  //   "content": [ { "type": "text", "text": "..." }, ... ],
  //   "stop_reason": "end_turn" | "tool_use" | "max_tokens",
  //   "usage": { "input_tokens": N, "output_tokens": N } }
  if (!providerResponse.is_object()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Anthropic response must be an object.");
  }

  if (providerResponse.contains("type") &&
      providerResponse.at("type").is_string() &&
      providerResponse.at("type").get<std::string>() == "error") {
    const std::string errMsg =
        providerResponse.contains("error") &&
                providerResponse.at("error").is_object()
            ? providerResponse.at("error").value("message",
                                                  std::string{"unknown error"})
            : "unknown error";
    return makeError(ModelErrorCode::ProviderUnavailable, errMsg);
  }

  if (!providerResponse.contains("content") ||
      !providerResponse.at("content").is_array()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Anthropic response is missing 'content' array.");
  }

  ModelResponse response;
  response.latencyMs = readLatencyMs(config_, providerResponse);
  response.finishReason =
      providerResponse.value("stop_reason", std::string{"end_turn"});

  if (!parseContentBlocks(providerResponse.at("content"), response.textOutput,
                          response.toolCalls)) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Anthropic content block parsing failed.",
                     response.latencyMs);
  }

  if (providerResponse.contains("usage") &&
      providerResponse.at("usage").is_object()) {
    const nlohmann::ordered_json& usage = providerResponse.at("usage");
    response.usageStats.promptTokens =
        usage.value("input_tokens", std::uint32_t{0U});
    response.usageStats.completionTokens =
        usage.value("output_tokens", std::uint32_t{0U});
    response.usageStats.totalTokens =
        response.usageStats.promptTokens + response.usageStats.completionTokens;
  }

  response.ok = true;
  return response;
}

}  // namespace ultra::ai::model::providers
