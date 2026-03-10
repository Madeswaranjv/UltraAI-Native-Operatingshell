#include "TokenBudgetManager.h"
//E:\Projects\Ultra\src\engine\context\TokenBudgetManager.cpp
#include <limits>

namespace ultra::engine::context {

TokenBudgetManager::TokenBudgetManager(const std::size_t tokenBudget)
    : tokenBudget_(tokenBudget) {}

std::size_t TokenBudgetManager::budget() const noexcept { return tokenBudget_; }

bool TokenBudgetManager::fits(const std::size_t estimatedTokens) const noexcept {
  return estimatedTokens <= tokenBudget_;
}

std::size_t TokenBudgetManager::estimateTextTokens(
    const std::string& payload) const {
  const std::size_t size = payload.size();
  if (size > (std::numeric_limits<std::size_t>::max() - 3U)) {
    return std::numeric_limits<std::size_t>::max() / 4U;
  }
  return (size + 3U) / 4U;
}

std::size_t TokenBudgetManager::estimatePayloadTokens(
    const nlohmann::ordered_json& payload) const {
  return estimateTextTokens(payload.dump());
}

}  // namespace ultra::engine::context
