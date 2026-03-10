#include "ModelAdapterRegistry.h"

#include "providers/DeepSeekAdapter.h"
#include "providers/OllamaAdapter.h"
#include "providers/OpenAIAdapter.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace ultra::ai::model {

namespace {

std::string readEnvVar(const std::string& name) {
#ifdef _WIN32
  char* buffer = nullptr;
  std::size_t len = 0U;
  std::string value;
  if (_dupenv_s(&buffer, &len, name.c_str()) == 0 && buffer != nullptr) {
    value = buffer;
    free(buffer);
  }
  return value;
#else
  const char* buffer = std::getenv(name.c_str());
  return buffer != nullptr ? std::string(buffer) : std::string{};
#endif
}

std::string expandEnvTokens(const std::string& value) {
  std::string expanded;
  std::size_t cursor = 0U;
  while (cursor < value.size()) {
    const std::size_t tokenStart = value.find("${", cursor);
    if (tokenStart == std::string::npos) {
      expanded.append(value.substr(cursor));
      break;
    }

    expanded.append(value.substr(cursor, tokenStart - cursor));
    const std::size_t tokenEnd = value.find('}', tokenStart + 2U);
    if (tokenEnd == std::string::npos) {
      expanded.append(value.substr(tokenStart));
      break;
    }

    const std::string token = value.substr(tokenStart + 2U,
                                           tokenEnd - tokenStart - 2U);
    expanded.append(readEnvVar(token));
    cursor = tokenEnd + 1U;
  }

  return expanded;
}

nlohmann::ordered_json sortJsonKeys(const nlohmann::ordered_json& value) {
  if (value.is_array()) {
    nlohmann::ordered_json sorted = nlohmann::ordered_json::array();
    for (const auto& item : value) {
      sorted.push_back(sortJsonKeys(item));
    }
    return sorted;
  }

  if (!value.is_object()) {
    return value;
  }

  std::vector<std::pair<std::string, nlohmann::ordered_json>> entries;
  entries.reserve(value.size());
  for (auto it = value.begin(); it != value.end(); ++it) {
    entries.emplace_back(it.key(), sortJsonKeys(it.value()));
  }

  std::sort(entries.begin(), entries.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  nlohmann::ordered_json sorted = nlohmann::ordered_json::object();
  for (auto& [key, item] : entries) {
    sorted[key] = std::move(item);
  }
  return sorted;
}

nlohmann::ordered_json expandEnvTokens(const nlohmann::ordered_json& value) {
  if (value.is_string()) {
    return expandEnvTokens(value.get<std::string>());
  }

  if (value.is_array()) {
    nlohmann::ordered_json expanded = nlohmann::ordered_json::array();
    for (const auto& item : value) {
      expanded.push_back(expandEnvTokens(item));
    }
    return expanded;
  }

  if (!value.is_object()) {
    return value;
  }

  nlohmann::ordered_json expanded = nlohmann::ordered_json::object();
  for (auto it = value.begin(); it != value.end(); ++it) {
    expanded[it.key()] = expandEnvTokens(it.value());
  }
  return expanded;
}

}  // namespace

ModelAdapterRegistry::ModelAdapterRegistry(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      configPath_(projectRoot_ / ".ultra" / "models.json") {
  configuration_["providers"] = nlohmann::ordered_json::object();
  registerBuiltIns();

  std::string ignored;
  reloadConfiguration(ignored);
}

void ModelAdapterRegistry::registerProvider(const std::string& providerName,
                                            AdapterFactory factory) {
  const std::string normalized = normalizeProviderName(providerName);
  if (normalized.empty()) {
    return;
  }
  factories_[normalized] = std::move(factory);
}

bool ModelAdapterRegistry::hasProvider(const std::string& providerName) const {
  return factories_.find(normalizeProviderName(providerName)) != factories_.end();
}

std::vector<std::string> ModelAdapterRegistry::listProviders() const {
  std::vector<std::string> providers;
  providers.reserve(factories_.size());
  for (const auto& [name, factory] : factories_) {
    (void)factory;
    providers.push_back(name);
  }
  return providers;
}

const std::filesystem::path& ModelAdapterRegistry::configPath() const noexcept {
  return configPath_;
}

nlohmann::ordered_json ModelAdapterRegistry::providerConfiguration(
    const std::string& providerName) const {
  const std::string normalized = normalizeProviderName(providerName);
  if (!configuration_.contains("providers") ||
      !configuration_.at("providers").is_object()) {
    return nlohmann::ordered_json::object();
  }

  const nlohmann::ordered_json& providers = configuration_.at("providers");
  auto it = providers.find(normalized);
  if (it == providers.end()) {
    return nlohmann::ordered_json::object();
  }
  return it.value();
}

bool ModelAdapterRegistry::reloadConfiguration(std::string& error) {
  configuration_ = nlohmann::ordered_json::object();
  configuration_["providers"] = nlohmann::ordered_json::object();

  if (!std::filesystem::exists(configPath_)) {
    error.clear();
    return true;
  }

  if (!std::filesystem::is_regular_file(configPath_)) {
    error = "Model adapter configuration exists but is not a file: " +
            configPath_.generic_string();
    return false;
  }

  std::ifstream input(configPath_);
  if (!input) {
    error = "Failed to open model adapter configuration: " +
            configPath_.generic_string();
    return false;
  }

  std::stringstream buffer;
  buffer << input.rdbuf();

  nlohmann::ordered_json parsed;
  try {
    parsed = nlohmann::ordered_json::parse(buffer.str());
  } catch (const nlohmann::json::exception& ex) {
    error = "Failed to parse model adapter configuration: " +
            std::string(ex.what());
    return false;
  }

  if (!parsed.is_object()) {
    error = "Model adapter configuration root must be an object.";
    return false;
  }

  if (!parsed.contains("providers") || !parsed.at("providers").is_object()) {
    error = "Model adapter configuration must contain object field 'providers'.";
    return false;
  }

  configuration_ = sortJsonKeys(expandEnvTokens(parsed));
  error.clear();
  return true;
}

std::unique_ptr<IModelAdapter> ModelAdapterRegistry::create(
    const std::string& providerName,
    std::string& error) const {
  const std::string normalized = normalizeProviderName(providerName);
  const auto factoryIt = factories_.find(normalized);
  if (factoryIt == factories_.end()) {
    error = "Model provider is not registered: " + providerName;
    return nullptr;
  }

  std::unique_ptr<IModelAdapter> adapter = factoryIt->second();
  if (!adapter) {
    error = "Model provider factory returned a null adapter: " + providerName;
    return nullptr;
  }

  const nlohmann::ordered_json config = providerConfiguration(normalized);
  if (!adapter->initialize(config, error)) {
    return nullptr;
  }

  error.clear();
  return adapter;
}

std::shared_ptr<ModelAdapterRegistry> ModelAdapterRegistry::createDefault(
    const std::filesystem::path& projectRoot) {
  return std::make_shared<ModelAdapterRegistry>(projectRoot);
}

std::string ModelAdapterRegistry::normalizeProviderName(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

void ModelAdapterRegistry::registerBuiltIns() {
  registerProvider("deepseek", []() {
    return std::make_unique<providers::DeepSeekAdapter>();
  });
  registerProvider("ollama", []() {
    return std::make_unique<providers::OllamaAdapter>();
  });
  registerProvider("openai", []() {
    return std::make_unique<providers::OpenAIAdapter>();
  });
}

}  // namespace ultra::ai::model
