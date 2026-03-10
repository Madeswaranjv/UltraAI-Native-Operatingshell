#pragma once

#include "IMultiModelOrchestrator.h"

#include "../model/ModelAdapterRegistry.h"

#include <external/json.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ultra::ai::orchestration {

struct OrchestrationDecision {
  std::string routingKey;
  std::string selectedProvider;
  std::vector<std::string> attemptedProviders;
  bool fallbackUsed{false};
};

class MultiModelOrchestrator final : public IMultiModelOrchestrator {
 public:
  explicit MultiModelOrchestrator(
      std::filesystem::path projectRoot = std::filesystem::current_path(),
      std::shared_ptr<model::ModelAdapterRegistry> registry = nullptr);

  [[nodiscard]] model::ModelResponse generate(
      const model::ModelRequest& request,
      const OrchestrationContext& context) override;

  bool reloadConfiguration(std::string& error);
  [[nodiscard]] const std::filesystem::path& configPath() const noexcept;
  [[nodiscard]] nlohmann::ordered_json routingConfiguration() const;
  [[nodiscard]] const OrchestrationDecision& lastDecision() const noexcept;

  [[nodiscard]] static std::shared_ptr<MultiModelOrchestrator> createDefault(
      const std::filesystem::path& projectRoot = std::filesystem::current_path());

 private:
  [[nodiscard]] std::vector<std::string> availableProviders(
      const OrchestrationContext& context) const;
  [[nodiscard]] std::vector<std::string> buildCandidateProviders(
      const OrchestrationContext& context) const;
  [[nodiscard]] std::string preferredProviderFor(
      const OrchestrationContext& context,
      const std::vector<std::string>& availableProviders) const;
  [[nodiscard]] std::string defaultProviderFor(
      const std::vector<std::string>& availableProviders) const;
  [[nodiscard]] bool isLocalProvider(const std::string& providerName) const;
  [[nodiscard]] static std::string normalizeProviderName(std::string value);

  std::filesystem::path projectRoot_;
  std::filesystem::path configPath_;
  std::shared_ptr<model::ModelAdapterRegistry> registry_;
  nlohmann::ordered_json routingConfig_ = nlohmann::ordered_json::object();
  OrchestrationDecision lastDecision_;
};

}  // namespace ultra::ai::orchestration
