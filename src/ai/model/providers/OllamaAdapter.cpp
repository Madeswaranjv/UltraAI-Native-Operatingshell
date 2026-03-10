#include "OllamaAdapter.h"

#include <algorithm>
#include <sstream>
#include <utility>

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

std::uint64_t readLatencyMs(const nlohmann::ordered_json& config,
                            const nlohmann::ordered_json& response) {
  if (response.contains("total_duration_ms") &&
      response.at("total_duration_ms").is_number()) {
    return response.at("total_duration_ms").get<std::uint64_t>();
  }
  if (response.contains("latency_ms") &&
      response.at("latency_ms").is_number()) {
    return response.at("latency_ms").get<std::uint64_t>();
  }
  if (config.contains("mock_latency_ms") &&
      config.at("mock_latency_ms").is_number()) {
    return config.at("mock_latency_ms").get<std::uint64_t>();
  }
  return 0U;
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

bool parseToolCalls(const nlohmann::ordered_json& rawCalls,
                    std::vector<ToolCall>& toolCalls) {
  if (!rawCalls.is_array()) {
    return false;
  }

  toolCalls.clear();
  for (const auto& rawCall : rawCalls) {
    const nlohmann::ordered_json function =
        rawCall.contains("function") && rawCall.at("function").is_object()
            ? rawCall.at("function")
            : rawCall;

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

}  // namespace

bool OllamaAdapter::initialize(const nlohmann::ordered_json& config,
                               std::string& error) {
  if (!config.is_null() && !config.is_object()) {
    error = "Ollama adapter configuration must be an object.";
    initialized_ = false;
    return false;
  }

  config_ = config.is_object() ? config : nlohmann::ordered_json::object();
  info_.providerName = "ollama";
  info_.modelName = config_.value("model", std::string{"llama3.2"});
  info_.supportsStreaming = true;
  info_.supportsToolCalls = true;
  info_.localProvider = true;
  info_.reasoningCapable = false;
  info_.contextWindow = config_.value("context_window", std::size_t{0U});

  const bool hasEndpoint =
      config_.contains("endpoint") && config_.at("endpoint").is_string() &&
      !config_.at("endpoint").get<std::string>().empty();
  const bool hasMockResponse = config_.contains("mock_response");
  if (!hasEndpoint && !hasMockResponse) {
    error = "Ollama adapter requires 'endpoint' or 'mock_response'.";
    initialized_ = false;
    return false;
  }

  initialized_ = true;
  error.clear();
  return true;
}

ModelResponse OllamaAdapter::generate(const ModelRequest& request) {
  if (!initialized_) {
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Ollama adapter is not initialized.");
  }

  (void)buildProviderRequest(request);

  if (!config_.contains("mock_response")) {
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Ollama transport is not configured.");
  }

  return translateResponse(config_.at("mock_response"));
}

ModelResponse OllamaAdapter::stream(const ModelRequest& request,
                                    const StreamCallback& onChunk) {
  if (config_.contains("mock_stream_chunks")) {
    const nlohmann::ordered_json& chunks = config_.at("mock_stream_chunks");
    if (!chunks.is_array()) {
      return makeError(ModelErrorCode::InvalidResponse,
                       "Ollama mock_stream_chunks must be an array.");
    }

    ModelResponse response;
    response.ok = true;
    response.finishReason = "stop";
    response.latencyMs = config_.value("mock_latency_ms", 0U);
    for (const auto& chunk : chunks) {
      if (!chunk.is_string()) {
        return makeError(ModelErrorCode::InvalidResponse,
                         "Ollama stream chunk must be a string.",
                         response.latencyMs);
      }
      const std::string text = chunk.get<std::string>();
      response.textOutput += text;
      if (onChunk) {
        onChunk(text);
      }
    }
    response.usageStats.promptTokens =
        static_cast<std::uint32_t>(tokenize(request.prompt).size());
    response.usageStats.completionTokens =
        static_cast<std::uint32_t>(tokenize(response.textOutput).size());
    response.usageStats.totalTokens = response.usageStats.promptTokens +
                                      response.usageStats.completionTokens;
    return response;
  }

  ModelResponse response = generate(request);
  if (response.ok && onChunk && !response.textOutput.empty()) {
    onChunk(response.textOutput);
  }
  return response;
}

std::vector<std::string> OllamaAdapter::tokenize(const std::string& text) const {
  return tokenizeWhitespace(text);
}

ModelInfo OllamaAdapter::modelInfo() const {
  return info_;
}

void OllamaAdapter::shutdown() {
  initialized_ = false;
}

nlohmann::ordered_json OllamaAdapter::buildProviderRequest(
    const ModelRequest& request) const {
  std::vector<std::string> tools = request.toolsAvailable;
  std::sort(tools.begin(), tools.end());
  tools.erase(std::unique(tools.begin(), tools.end()), tools.end());

  nlohmann::ordered_json toolsPayload = nlohmann::ordered_json::array();
  for (const std::string& tool : tools) {
    nlohmann::ordered_json function = nlohmann::ordered_json::object();
    function["arguments"] = nlohmann::ordered_json::object();
    function["name"] = tool;

    nlohmann::ordered_json item = nlohmann::ordered_json::object();
    item["function"] = std::move(function);
    toolsPayload.push_back(std::move(item));
  }

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["context"] = request.contextPayload;
  payload["model"] = info_.modelName;
  payload["options"] = {{"num_predict", request.maxTokens},
                        {"temperature", request.temperature}};
  payload["prompt"] = request.prompt;
  payload["stream"] = false;
  payload["system"] = request.systemPrompt;
  payload["tools"] = std::move(toolsPayload);
  return payload;
}

ModelResponse OllamaAdapter::translateResponse(
    const nlohmann::ordered_json& providerResponse) const {
  if (!providerResponse.is_object()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Ollama response must be an object.");
  }

  ModelResponse response;
  response.ok = true;
  response.textOutput = providerResponse.value("response", std::string{});
  if (response.textOutput.empty() && providerResponse.contains("message") &&
      providerResponse.at("message").is_object()) {
    response.textOutput =
        providerResponse.at("message").value("content", std::string{});
  }

  response.finishReason =
      providerResponse.value("done_reason",
                             providerResponse.value("finish_reason",
                                                    std::string{"stop"}));
  response.latencyMs = readLatencyMs(config_, providerResponse);
  response.usageStats.promptTokens =
      providerResponse.value("prompt_eval_count", std::uint32_t{0U});
  response.usageStats.completionTokens =
      providerResponse.value("eval_count", std::uint32_t{0U});
  response.usageStats.totalTokens =
      providerResponse.value("total_tokens",
                             response.usageStats.promptTokens +
                                 response.usageStats.completionTokens);

  if (providerResponse.contains("tool_calls")) {
    if (!parseToolCalls(providerResponse.at("tool_calls"), response.toolCalls)) {
      return makeError(ModelErrorCode::InvalidResponse,
                       "Ollama tool call payload is invalid.",
                       response.latencyMs);
    }
  }

  return response;
}

}  // namespace ultra::ai::model::providers
