#pragma once

#include "../IModelAdapter.h"

namespace ultra::ai::model::providers {

class OpenAIAdapter final : public IModelAdapter {
 public:
  bool initialize(const nlohmann::ordered_json& config,
                  std::string& error) override;
  [[nodiscard]] ModelResponse generate(const ModelRequest& request) override;
  [[nodiscard]] ModelResponse stream(const ModelRequest& request,
                                    const StreamCallback& onChunk) override;
  [[nodiscard]] std::vector<std::string> tokenize(
      const std::string& text) const override;
  [[nodiscard]] ModelInfo modelInfo() const override;
  void shutdown() override;

 private:
  [[nodiscard]] nlohmann::ordered_json buildProviderRequest(
      const ModelRequest& request) const;
  [[nodiscard]] ModelResponse translateResponse(
      const nlohmann::ordered_json& providerResponse) const;

  nlohmann::ordered_json config_ = nlohmann::ordered_json::object();
  ModelInfo info_{};
  bool initialized_{false};
};

}  // namespace ultra::ai::model::providers
