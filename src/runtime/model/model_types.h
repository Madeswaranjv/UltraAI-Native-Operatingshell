#pragma once

#include <external/json.hpp>

#include <string>

namespace ultra::runtime::model {

struct ModelRequest {
  std::string prompt;
  nlohmann::ordered_json context = nlohmann::ordered_json::object();
  std::string intentReference;
  nlohmann::ordered_json constraints = nlohmann::ordered_json::object();
};

struct ModelResponse {
  bool ok{false};
  std::string strategyText;
  nlohmann::ordered_json structuredOutput = nlohmann::ordered_json::object();
  std::string errorMessage;
};

}  // namespace ultra::runtime::model

