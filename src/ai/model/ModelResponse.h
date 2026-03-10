#pragma once

#include <external/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace ultra::ai::model {

enum class ModelErrorCode : std::uint8_t {
  None = 0U,
  ModelTimeout = 1U,
  InvalidResponse = 2U,
  ProviderUnavailable = 3U,
  RateLimited = 4U
};

struct ToolCall {
  std::string name;
  nlohmann::ordered_json arguments = nlohmann::ordered_json::object();
};

struct UsageStats {
  std::uint32_t promptTokens{0U};
  std::uint32_t completionTokens{0U};
  std::uint32_t totalTokens{0U};
};

struct ModelResponse {
  bool ok{false};
  std::string textOutput;
  std::vector<ToolCall> toolCalls;
  UsageStats usageStats;
  std::string finishReason;
  std::uint64_t latencyMs{0U};
  ModelErrorCode errorCode{ModelErrorCode::None};
  std::string errorMessage;
};

inline std::string toString(const ModelErrorCode code) {
  switch (code) {
    case ModelErrorCode::None:
      return "NONE";
    case ModelErrorCode::ModelTimeout:
      return "MODEL_TIMEOUT";
    case ModelErrorCode::InvalidResponse:
      return "INVALID_RESPONSE";
    case ModelErrorCode::ProviderUnavailable:
      return "PROVIDER_UNAVAILABLE";
    case ModelErrorCode::RateLimited:
      return "RATE_LIMITED";
  }
  return "PROVIDER_UNAVAILABLE";
}

}  // namespace ultra::ai::model
