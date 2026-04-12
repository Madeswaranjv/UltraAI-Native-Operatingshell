#pragma once

#include "IModelAdapter.h"

#include <external/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::ai::model {

class ModelAdapterRegistry {
 public:
  using AdapterFactory = std::function<std::unique_ptr<IModelAdapter>()>;

  explicit ModelAdapterRegistry(
      std::filesystem::path projectRoot = std::filesystem::current_path());

  void registerProvider(const std::string& providerName, AdapterFactory factory);

  [[nodiscard]] bool hasProvider(const std::string& providerName) const;
  [[nodiscard]] std::vector<std::string> listProviders() const;
  [[nodiscard]] const std::filesystem::path& configPath() const noexcept;
  [[nodiscard]] nlohmann::ordered_json providerConfiguration(
      const std::string& providerName) const;
  [[nodiscard]] std::vector<std::string> getRoleProviders(
      const std::string& role) const;
  [[nodiscard]] std::string primaryProviderForRole(
      const std::string& role) const;

  bool reloadConfiguration(std::string& error);
  [[nodiscard]] std::unique_ptr<IModelAdapter> create(
      const std::string& providerName,
      std::string& error) const;

  [[nodiscard]] static std::shared_ptr<ModelAdapterRegistry> createDefault(
      const std::filesystem::path& projectRoot = std::filesystem::current_path());

 private:
  static std::string normalizeProviderName(std::string value);
  static std::string normalizeRoleName(std::string value);
  void registerBuiltIns();

  std::filesystem::path projectRoot_;
  std::filesystem::path configPath_;
  std::map<std::string, AdapterFactory> factories_;
  nlohmann::ordered_json configuration_ = nlohmann::ordered_json::object();
  std::unordered_map<std::string, std::vector<std::string>> roleMap_;
};

}  // namespace ultra::ai::model
