#include "GeminiAdapter.h"

#include <algorithm>
#include <sstream>
#include <iostream>
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

// Gemini response uses candidates[].content.parts[].
// Each part has either "text" or "functionCall".
bool parseGeminiParts(const nlohmann::ordered_json& parts,
                      std::string& textOutput,
                      std::vector<ToolCall>& toolCalls) {
  if (!parts.is_array()) {
    return false;
  }

  toolCalls.clear();
  textOutput.clear();

  for (const auto& part : parts) {
    if (!part.is_object()) {
      return false;
    }
    if (part.contains("text") && part.at("text").is_string()) {
      textOutput += part.at("text").get<std::string>();
    } else if (part.contains("functionCall") &&
               part.at("functionCall").is_object()) {
      const nlohmann::ordered_json& fc = part.at("functionCall");
      if (!fc.contains("name") || !fc.at("name").is_string()) {
        return false;
      }
      ToolCall toolCall;
      toolCall.name = fc.at("name").get<std::string>();
      const nlohmann::ordered_json argsValue =
          fc.contains("args") ? fc.at("args") : nlohmann::ordered_json::object();
      if (!parseArguments(argsValue, toolCall.arguments)) {
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

bool GeminiAdapter::initialize(const nlohmann::ordered_json& config,
                               std::string& error) {
  if (!config.is_null() && !config.is_object()) {
    error = "Gemini adapter configuration must be an object.";
    initialized_ = false;
    return false;
  }

  config_ = config.is_object() ? config : nlohmann::ordered_json::object();
  info_.providerName = "gemini";
  info_.modelName = defaultConfiguredModelName(config_, "gemini-2.0-flash");
  info_.supportsStreaming = true;
  info_.supportsToolCalls = true;
  info_.localProvider = false;
  info_.reasoningCapable =
      config_.value("reasoning_capable", false);
  info_.contextWindow =
      config_.value("context_window", std::size_t{1000000U});

  const bool hasApiKey =
      config_.contains("api_key") && config_.at("api_key").is_string() &&
      !config_.at("api_key").get<std::string>().empty();
  const bool hasMockResponse = config_.contains("mock_response");
  if (!hasApiKey && !hasMockResponse) {
    error = "Gemini adapter requires 'api_key' or 'mock_response'.";
    initialized_ = false;
    return false;
  }

  initialized_ = true;
  error.clear();
  return true;
}

ModelResponse GeminiAdapter::generate(const ModelRequest& request) {
  if (!initialized_) {
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Gemini adapter is not initialized.");
  }

  if (config_.contains("mock_response")) {
    return translateResponse(config_.at("mock_response"));
  }

  const std::string apiKey  = config_.value("api_key", std::string{});
  const std::string baseUrl =
      config_.value("base_url",
                    std::string{"https://generativelanguage.googleapis.com"});
  const std::string modelName =
      configuredModelNameForRequest(config_, request, info_.modelName);
  // Gemini passes the key as a query parameter, not a header.
  const std::string url = baseUrl + "/v1beta/models/" + modelName +
                          ":generateContent?key=" + apiKey;

  const nlohmann::ordered_json payload = buildProviderRequest(request);
  const std::string body = payload.dump();

  const http::HttpResponse httpResp = http::httpPost(url, body, {});

  if (!httpResp.ok) {
    if (httpResp.statusCode == 0) {
      return makeError(ModelErrorCode::ProviderUnavailable,
                       "Gemini HTTP transport error: " + httpResp.errorMessage);
    }
    try {
      const nlohmann::ordered_json errJson =
          nlohmann::ordered_json::parse(httpResp.body);
      if (errJson.contains("error") && errJson.at("error").is_object()) {
        return makeError(ModelErrorCode::ProviderUnavailable,
                         "Gemini error: " +
                         errJson.at("error").value("message", std::string{"unknown"}));
      }
    } catch (...) {}
    return makeError(ModelErrorCode::ProviderUnavailable,
                     "Gemini returned HTTP " + std::to_string(httpResp.statusCode));
  }

  try {
    const nlohmann::ordered_json responseJson =
        nlohmann::ordered_json::parse(httpResp.body);
    return translateResponse(responseJson);
  } catch (const nlohmann::json::exception& ex) {
    return makeError(ModelErrorCode::InvalidResponse,
                     std::string("Gemini response JSON parse failed: ") + ex.what());
  }
}

ModelResponse GeminiAdapter::stream(const ModelRequest& request,
                                    const StreamCallback& onChunk) {
  ModelResponse response = generate(request);
  if (response.ok && onChunk && !response.textOutput.empty()) {
    onChunk(response.textOutput);
  }
  return response;
}

std::vector<std::string> GeminiAdapter::tokenize(const std::string& text) const {
  return tokenizeWhitespace(text);
}

ModelInfo GeminiAdapter::modelInfo() const {
  return info_;
}

void GeminiAdapter::shutdown() {
  initialized_ = false;
}

nlohmann::ordered_json GeminiAdapter::buildProviderRequest(
    const ModelRequest& request) const {
  std::vector<std::string> tools = providerSchemaToolNames(request);

  // Gemini uses contents[].parts[] and a separate systemInstruction field.
  nlohmann::ordered_json userParts = nlohmann::ordered_json::array();
  userParts.push_back({{"text", request.prompt}});

  nlohmann::ordered_json contents = nlohmann::ordered_json::array();
  contents.push_back({{"parts", std::move(userParts)}, {"role", "user"}});

  // Function declarations for tool use.
  nlohmann::ordered_json functionDeclarations = nlohmann::ordered_json::array();
  for (const std::string& tool : tools) {
    nlohmann::ordered_json decl = nlohmann::ordered_json::object();
    decl["description"] = tool;
    decl["name"] = tool;
    decl["parameters"] = {{"properties", nlohmann::ordered_json::object()},
                           {"type", "object"}};
    functionDeclarations.push_back(std::move(decl));
  }

  nlohmann::ordered_json generationConfig = nlohmann::ordered_json::object();
  if (request.maxTokens > 0U) {
    generationConfig["maxOutputTokens"] = request.maxTokens;
  }
  generationConfig["temperature"] = request.temperature;

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["contents"] = std::move(contents);
  payload["context_payload"] = request.contextPayload;
  payload["generationConfig"] = std::move(generationConfig);
  payload["tools"] = nlohmann::ordered_json::array(
      {nlohmann::ordered_json{{"functionDeclarations",
                               std::move(functionDeclarations)}}});
  if (!request.systemPrompt.empty()) {
    payload["systemInstruction"] = {
        {"parts", nlohmann::ordered_json::array(
                      {nlohmann::ordered_json{{"text", request.systemPrompt}}})}};
  }
  return payload;
}

ModelResponse GeminiAdapter::translateResponse(
    const nlohmann::ordered_json& providerResponse) const {
  // Gemini response shape:
  // { "candidates": [ { "content": { "parts": [...], "role": "model" },
  //                     "finishReason": "STOP" | "MAX_TOKENS" | ... } ],
  //   "usageMetadata": { "promptTokenCount": N, "candidatesTokenCount": N,
  //                      "totalTokenCount": N } }
  if (!providerResponse.is_object()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Gemini response must be an object.");
  }

  if (providerResponse.contains("error") &&
      providerResponse.at("error").is_object()) {
    const std::string errMsg =
        providerResponse.at("error").value("message", std::string{"unknown error"});
    return makeError(ModelErrorCode::ProviderUnavailable, errMsg);
  }

  if (!providerResponse.contains("candidates") ||
      !providerResponse.at("candidates").is_array() ||
      providerResponse.at("candidates").empty()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Gemini response must contain non-empty 'candidates' array.");
  }

  const nlohmann::ordered_json& candidate =
      providerResponse.at("candidates").front();
  if (!candidate.is_object() || !candidate.contains("content") ||
      !candidate.at("content").is_object()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Gemini candidate is missing 'content'.");
  }

  const nlohmann::ordered_json& content = candidate.at("content");
  if (!content.contains("parts") || !content.at("parts").is_array()) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Gemini content is missing 'parts' array.");
  }

  ModelResponse response;
  response.latencyMs = readLatencyMs(config_, providerResponse);

  // Gemini finish reasons are uppercase: "STOP", "MAX_TOKENS", etc.
  const std::string finishReason =
      candidate.value("finishReason", std::string{"STOP"});
  response.finishReason = finishReason == "STOP" ? "stop" : finishReason;

  if (!parseGeminiParts(content.at("parts"), response.textOutput,
                        response.toolCalls)) {
    return makeError(ModelErrorCode::InvalidResponse,
                     "Gemini parts parsing failed.", response.latencyMs);
  }

  std::cerr << "[DEBUG] Gemini text_output size = "
            << response.textOutput.size() << std::endl;

  if (providerResponse.contains("usageMetadata") &&
      providerResponse.at("usageMetadata").is_object()) {
    const nlohmann::ordered_json& usage = providerResponse.at("usageMetadata");
    response.usageStats.promptTokens =
        usage.value("promptTokenCount", std::uint32_t{0U});
    response.usageStats.completionTokens =
        usage.value("candidatesTokenCount", std::uint32_t{0U});
    response.usageStats.totalTokens =
        usage.value("totalTokenCount",
                    response.usageStats.promptTokens +
                        response.usageStats.completionTokens);
  }

  response.ok = true;
  return response;
}

}  // namespace ultra::ai::model::providers
