#pragma once

#include <external/json.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace ultra::ai::model {

struct ModelRequest {
  std::string prompt;
  std::string systemPrompt;
  double temperature{0.0};
  std::size_t maxTokens{0U};
  std::vector<std::string> toolsAvailable;
  nlohmann::ordered_json contextPayload = nlohmann::ordered_json::object();
};

}  // namespace ultra::ai::model
