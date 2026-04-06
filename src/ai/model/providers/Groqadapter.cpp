#include "GroqAdapter.h"

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

// Groq is OpenAI-compatible — same choices[].message.content shape.
bool parseToolCalls(const nlohmann::ordered_json& rawCalls,
                    std::vector<ToolCall>& toolCalls) {
  if (!rawCalls.is_array()) {
    return false;
  }
  toolCalls.clear();
  for (const auto& rawCall : rawCalls) {
    if (!rawCall.is_object() || !rawCall.contains("function") ||
        !rawCall.at("function").is_object()) {
      return false;
    }
    const nlohmann::ordered_json& function = rawCall.at("function");
    if (!function.contains("name") || !function.at("name").is_string()) {
      return false;
    }
    ToolCall toolCall;
    toolCall.name = function.at("name").get<std::string>();
    const nlohmann::ordered_json argumentsValue =
        function.contains("arguments") ? function.at("arguments")
                                       : nlohmann::ordered_json::object();
    if (!parseArguments(argumentsValue, toolCall.arguments)) {
      return false;
    }
    toolCalls.push_back(std::move(toolCall));
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
  // Groq includes x-groq-processing-time in headers; in the body it's absent.
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

bool GroqAdapter::initialize(const nlohmann::ordered_json& config,
                             std::string& error) {
  if (!config.is_null() && !config.is_object()) {
    error = "Groq adapter configuration must be an object.";
    initialized_ = false;
    return false;
  }

  config_ = config.is_object() ? config : nlohmann::ordered_json::object();
  info_.providerName = "groq";
  // Default to llama-3.3-70b — Groq's fastest high-quality model.
  info_.modelName =
      config_.value("model", std::string{"llama-3.3-70b-versatile"});
  info_.supportsStreaming = true;
  info_.supportsToolCalls = true;
  info_.localProvider = false;
  info_.reasoningCapable = false;
  info_.contextWindow =
      config_.value("context_window", std::size_t{128000U});

  const bool hasApiKey =
      config_.contains("api_key") && config_.at("api_key").is_string() &&
      !config_.at("api_key").get<std::string>().empty();
  const bool hasMockResponse = config_.contains("mock_response");
  if (!hasApiKey && !hasMockResponse) {
    error = "Groq adapter requires 'api_key' or 'mock_response'.";
    initialized_ = false;
    return false;
  }

  initialized_ = true;
  error.clear();
  return true;
}

ModelResponse GroqAdapter::generate(const ModelRequest& request) {
  if (!initialized_) {
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Groq adapter is not initialized.");
  }

  if (config_.contains("mock_response")) {
    return translateResponse(config_.at("mock_response"));
  }

  const std::string apiKey  = config_.value("api_key", std::string{});
  const std::string baseUrl =
      config_.value("base_url", std::string{"https://api.groq.com"});
  const std::string url = baseUrl + "/openai/v1/chat/completions";

  const nlohmann::ordered_json payload = buildProviderRequest(request);
  const std::string body = payload.dump();

  const std::unordered_map<std::string, std::string> headers = {
      {"Authorization", "Bearer " + apiKey},
  };

  const http::HttpResponse httpResp = http::httpPost(url, body, headers);

  if (!httpResp.ok) {
    if (httpResp.statusCode == 0) {
      return makeError(ModelErrorCode::ProviderUnavailable,
                       "Groq HTTP transport error: " + httpResp.errorMessage);
    }
    try {
      const nlohmann::ordered_json errJson =
          nlohmann::ordered_json::parse(httpResp.body);
      if (errJson.contains("error") && errJson.at("error").is_object()) {
        return makeError(ModelErrorCode::ProviderUnavailable,
                         "Groq error: " +
                         errJson.at("error").value("message", std::string{"unknown"}));
      }
    } catch (...) {}
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Groq returned HTTP " + std::to_string(httpResp.statusCode));
  }

  try {
    const nlohmann::ordered_json responseJson =
        nlohmann::ordered_json::parse(httpResp.body);
    return translateResponse(responseJson);
  } catch (const nlohmann::json::exception& ex) {
    return makeError(ModelErrorCode::InvalidResponse,
                     std::string("Groq response JSON parse failed: ") + ex.what());
  }
}

ModelResponse GroqAdapter::stream(const ModelRequest& request,
                                  const StreamCallback& onChunk) {
  ModelResponse response = generate(request);
  if (response.ok && onChunk && !response.textOutput.empty()) {
    onChunk(response.textOutput);
  }
  return response;
}

std::vector<std::string> GroqAdapter::tokenize(const std::string& text) const {
  return tokenizeWhitespace(text);
}

ModelInfo GroqAdapter::modelInfo() const {
  return info_;
}

void GroqAdapter::shutdown() {
  initialized_ = false;
}

nlohmann::ordered_json GroqAdapter::buildProviderRequest(
    const ModelRequest& request) const {
  std::vector<std::string> tools = providerSchemaToolNames(request);

  // Groq is OpenAI-compatible — same messages format.
  nlohmann::ordered_json messages = nlohmann::ordered_json::array();
  if (!request.systemPrompt.empty()) {
    messages.push_back({{"content", request.systemPrompt}, {"role", "system"}});
  }
  messages.push_back({{"content", request.prompt}, {"role", "user"}});

  nlohmann::ordered_json toolPayload = nlohmann::ordered_json::array();
  for (const std::string& tool : tools) {
    nlohmann::ordered_json function = nlohmann::ordered_json::object();
    function["name"] = tool;
    function["parameters"] = nlohmann::ordered_json::object();

    nlohmann::ordered_json item = nlohmann::ordered_json::object();
    item["function"] = std::move(function);
    item["type"] = "function";
    toolPayload.push_back(std::move(item));
  }

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["context_payload"] = request.contextPayload;
  payload["messages"] = std::move(messages);
  payload["model"] = info_.modelName;
  payload["temperature"] = request.temperature;
  payload["tools"] = std::move(toolPayload);
  if (request.maxTokens > 0U) {
    payload["max_tokens"] = request.maxTokens;
  }
  return payload;
}

ModelResponse GroqAdapter::translateResponse(
    const nlohmann::ordered_json& providerResponse) const {
  // Groq uses the identical OpenAI response shape.
  if (!providerResponse.is_object() || !providerResponse.contains("choices") ||
      !providerResponse.at("choices").is_array() ||
      providerResponse.at("choices").empty()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Groq response must contain non-empty array 'choices'.");
  }

  const nlohmann::ordered_json& choice = providerResponse.at("choices").front();
  if (!choice.is_object() || !choice.contains("message") ||
      !choice.at("message").is_object()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Groq response is missing message payload.");
  }

  const nlohmann::ordered_json& message = choice.at("message");
  ModelResponse response;
  response.ok = true;
  response.textOutput = message.value("content", std::string{});
  response.finishReason = choice.value("finish_reason", std::string{"stop"});
  response.latencyMs = readLatencyMs(config_, providerResponse);

  if (providerResponse.contains("usage") &&
      providerResponse.at("usage").is_object()) {
    const nlohmann::ordered_json& usage = providerResponse.at("usage");
    response.usageStats.promptTokens =
        usage.value("prompt_tokens", std::uint32_t{0U});
    response.usageStats.completionTokens =
        usage.value("completion_tokens", std::uint32_t{0U});
    response.usageStats.totalTokens =
        usage.value("total_tokens", response.usageStats.promptTokens +
                                        response.usageStats.completionTokens);
  }

  if (message.contains("tool_calls")) {
    if (!parseToolCalls(message.at("tool_calls"), response.toolCalls)) {
      return makeError(ModelErrorCode::InvalidResponse,
                       "Groq tool call payload is invalid.", response.latencyMs);
    }
  }

  return response;
}

}  // namespace ultra::ai::model::providers
