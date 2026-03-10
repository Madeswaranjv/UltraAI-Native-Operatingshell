#pragma once

#include <external/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ultra::ai::orchestration {

enum class TaskType : std::uint8_t {
  Planning = 0U,
  Reasoning = 1U,
  Coding = 2U,
  Analysis = 3U
};

enum class TaskComplexity : std::uint8_t {
  Low = 0U,
  Medium = 1U,
  High = 2U
};

enum class TaskPriority : std::uint8_t {
  Background = 0U,
  Standard = 1U,
  Urgent = 2U
};

inline std::string toString(const TaskType type) {
  switch (type) {
    case TaskType::Planning:
      return "planning";
    case TaskType::Reasoning:
      return "reasoning";
    case TaskType::Coding:
      return "coding";
    case TaskType::Analysis:
      return "analysis";
  }
  return "analysis";
}

inline std::optional<TaskType> taskTypeFromString(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  if (value == "planning") {
    return TaskType::Planning;
  }
  if (value == "reasoning") {
    return TaskType::Reasoning;
  }
  if (value == "coding" || value == "code_generation") {
    return TaskType::Coding;
  }
  if (value == "analysis") {
    return TaskType::Analysis;
  }
  return std::nullopt;
}

inline std::string toString(const TaskComplexity complexity) {
  switch (complexity) {
    case TaskComplexity::Low:
      return "low";
    case TaskComplexity::Medium:
      return "medium";
    case TaskComplexity::High:
      return "high";
  }
  return "medium";
}

inline std::string toString(const TaskPriority priority) {
  switch (priority) {
    case TaskPriority::Background:
      return "background";
    case TaskPriority::Standard:
      return "standard";
    case TaskPriority::Urgent:
      return "urgent";
  }
  return "standard";
}

struct OrchestrationContext {
  TaskType taskType{TaskType::Analysis};
  TaskComplexity complexity{TaskComplexity::Medium};
  TaskPriority priority{TaskPriority::Standard};
  std::uint64_t latencyBudgetMs{0U};
  std::size_t tokenBudget{0U};
  std::vector<std::string> availableModels;
};

inline std::vector<std::string> normalizedAvailableModels(
    const OrchestrationContext& context) {
  std::vector<std::string> models = context.availableModels;
  for (std::string& model : models) {
    std::transform(model.begin(), model.end(), model.begin(),
                   [](unsigned char ch) {
                     return static_cast<char>(std::tolower(ch));
                   });
  }
  std::sort(models.begin(), models.end());
  models.erase(std::unique(models.begin(), models.end()), models.end());
  return models;
}

inline nlohmann::ordered_json toJson(const OrchestrationContext& context) {
  nlohmann::ordered_json models = nlohmann::ordered_json::array();
  for (const std::string& model : normalizedAvailableModels(context)) {
    models.push_back(model);
  }

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["available_models"] = std::move(models);
  payload["complexity"] = toString(context.complexity);
  payload["latency_budget_ms"] = context.latencyBudgetMs;
  payload["priority"] = toString(context.priority);
  payload["task_type"] = toString(context.taskType);
  payload["token_budget"] = context.tokenBudget;
  return payload;
}

}  // namespace ultra::ai::orchestration
