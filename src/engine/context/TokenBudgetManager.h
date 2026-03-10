#pragma once

#include <external/json.hpp>
//E:\Projects\Ultra\src\engine\context\TokenBudgetManager.h
#include <cstddef>
#include <string>

namespace ultra::engine::context {

class TokenBudgetManager {
 public:
  explicit TokenBudgetManager(std::size_t tokenBudget);

  [[nodiscard]] std::size_t budget() const noexcept;
  [[nodiscard]] bool fits(std::size_t estimatedTokens) const noexcept;
  [[nodiscard]] std::size_t estimateTextTokens(const std::string& payload) const;
  [[nodiscard]] std::size_t estimatePayloadTokens(
      const nlohmann::ordered_json& payload) const;

 private:
  std::size_t tokenBudget_{0U};
};

}  // namespace ultra::engine::context
