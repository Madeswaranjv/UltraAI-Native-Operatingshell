#include "MultiModelOrchestrator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace ultra::ai::orchestration {

namespace {

struct FailureRecord {
  std::string provider;
  model::ModelErrorCode code{model::ModelErrorCode::ProviderUnavailable};
  std::string message;
  std::uint64_t latencyMs{0U};
};

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

void pushUnique(std::vector<std::string>& values, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

bool containsValue(const std::vector<std::string>& values,
                   const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
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

nlohmann::ordered_json defaultRoutingConfiguration() {
  nlohmann::ordered_json routing = nlohmann::ordered_json::object();
  routing["analysis"] = "openai";
  routing["coding"] = "ollama";
  routing["planning"] = "deepseek";
  routing["reasoning"] = "deepseek";

  nlohmann::ordered_json payload = nlohmann::ordered_json::object();
  payload["routing"] = std::move(routing);
  return payload;
}

bool isRetryableFailure(const model::ModelErrorCode code) {
  switch (code) {
    case model::ModelErrorCode::ProviderUnavailable:
    case model::ModelErrorCode::ModelTimeout:
    case model::ModelErrorCode::RateLimited:
    case model::ModelErrorCode::InvalidResponse:
      return true;
    case model::ModelErrorCode::None:
      return false;
  }
  return true;
}

model::ModelErrorCode collapseFailureCode(
    const std::vector<FailureRecord>& failures) {
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::RateLimited) {
      return failure.code;
    }
  }
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::ModelTimeout) {
      return failure.code;
    }
  }
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::InvalidResponse) {
      return failure.code;
    }
  }
  for (const FailureRecord& failure : failures) {
    if (failure.code == model::ModelErrorCode::ProviderUnavailable) {
      return failure.code;
    }
  }
  return model::ModelErrorCode::ProviderUnavailable;
}

std::string buildFailureMessage(const std::vector<FailureRecord>& failures) {
  if (failures.empty()) {
    return "No model providers were available for orchestration.";
  }

  std::ostringstream stream;
  stream << "No model provider succeeded. ";
  for (std::size_t index = 0U; index < failures.size(); ++index) {
    const FailureRecord& failure = failures[index];
    if (index != 0U) {
      stream << " | ";
    }
    stream << failure.provider << " [" << model::toString(failure.code) << "]";
    if (!failure.message.empty()) {
      stream << ": " << failure.message;
    }
  }
  return stream.str();
}

}  // namespace

MultiModelOrchestrator::MultiModelOrchestrator(
    std::filesystem::path projectRoot,
    std::shared_ptr<model::ModelAdapterRegistry> registry)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      configPath_(projectRoot_ / ".ultra" / "model_routing.json"),
      registry_(registry != nullptr ? std::move(registry)
                                    : model::ModelAdapterRegistry::createDefault(
                                          projectRoot_)),
      routingConfig_(defaultRoutingConfiguration()) {
  std::string ignored;
  reloadConfiguration(ignored);
}

model::ModelResponse MultiModelOrchestrator::generate(
    const model::ModelRequest& request,
    const OrchestrationContext& context) {
  lastDecision_ = OrchestrationDecision{};
  lastDecision_.routingKey = toString(context.taskType);
  lastDecision_.attemptedProviders = buildCandidateProviders(context);

  if (!registry_) {
    model::ModelResponse response;
    response.ok = false;
    response.errorCode = model::ModelErrorCode::ProviderUnavailable;
    response.errorMessage = "Model adapter registry is unavailable.";
    return response;
  }

  std::vector<FailureRecord> failures;
  failures.reserve(lastDecision_.attemptedProviders.size());

  for (std::size_t index = 0U; index < lastDecision_.attemptedProviders.size();
       ++index) {
    const std::string& provider = lastDecision_.attemptedProviders[index];
    std::string error;
    std::unique_ptr<model::IModelAdapter> adapter =
        registry_->create(provider, error);
    if (!adapter) {
      failures.push_back(FailureRecord{
          provider,
          model::ModelErrorCode::ProviderUnavailable,
          error.empty() ? "Provider is unavailable." : error,
          0U,
      });
      continue;
    }

    model::ModelResponse response = adapter->generate(request);
    adapter->shutdown();
    if (response.ok) {
      lastDecision_.selectedProvider = provider;
      lastDecision_.fallbackUsed = index > 0U;
      return response;
    }

    const model::ModelErrorCode code =
        response.errorCode == model::ModelErrorCode::None
            ? model::ModelErrorCode::InvalidResponse
            : response.errorCode;
    failures.push_back(FailureRecord{
        provider,
        code,
        response.errorMessage.empty()
            ? "Provider returned an unsuccessful response."
            : response.errorMessage,
        response.latencyMs,
    });

    if (!isRetryableFailure(code)) {
      break;
    }
  }

  model::ModelResponse response;
  response.ok = false;
  response.errorCode = collapseFailureCode(failures);
  response.errorMessage = buildFailureMessage(failures);
  response.finishReason = "fallback_exhausted";
  for (const FailureRecord& failure : failures) {
    response.latencyMs += failure.latencyMs;
  }
  lastDecision_.fallbackUsed = failures.size() > 1U;
  return response;
}

bool MultiModelOrchestrator::reloadConfiguration(std::string& error) {
  routingConfig_ = defaultRoutingConfiguration();

  if (!std::filesystem::exists(configPath_)) {
    error.clear();
    return true;
  }

  if (!std::filesystem::is_regular_file(configPath_)) {
    error = "Model routing configuration exists but is not a file: " +
            configPath_.generic_string();
    return false;
  }

  std::ifstream input(configPath_);
  if (!input) {
    error = "Failed to open model routing configuration: " +
            configPath_.generic_string();
    return false;
  }

  std::stringstream buffer;
  buffer << input.rdbuf();

  nlohmann::ordered_json parsed;
  try {
    parsed = nlohmann::ordered_json::parse(buffer.str());
  } catch (const nlohmann::json::exception& ex) {
    error = "Failed to parse model routing configuration: " +
            std::string(ex.what());
    return false;
  }

  if (!parsed.is_object()) {
    error = "Model routing configuration root must be an object.";
    return false;
  }

  if (!parsed.contains("routing") || !parsed.at("routing").is_object()) {
    error = "Model routing configuration must contain object field 'routing'.";
    return false;
  }

  nlohmann::ordered_json merged = defaultRoutingConfiguration();
  for (auto it = parsed.at("routing").begin(); it != parsed.at("routing").end();
       ++it) {
    if (!it.value().is_string()) {
      error = "Routing target for key '" + it.key() + "' must be a string.";
      return false;
    }
    merged["routing"][lowerAscii(it.key())] =
        normalizeProviderName(it.value().get<std::string>());
  }

  routingConfig_ = sortJsonKeys(merged);
  error.clear();
  return true;
}

const std::filesystem::path& MultiModelOrchestrator::configPath() const noexcept {
  return configPath_;
}

nlohmann::ordered_json MultiModelOrchestrator::routingConfiguration() const {
  return routingConfig_;
}

const OrchestrationDecision& MultiModelOrchestrator::lastDecision() const noexcept {
  return lastDecision_;
}

std::shared_ptr<MultiModelOrchestrator> MultiModelOrchestrator::createDefault(
    const std::filesystem::path& projectRoot) {
  return std::make_shared<MultiModelOrchestrator>(projectRoot);
}

std::vector<std::string> MultiModelOrchestrator::availableProviders(
    const OrchestrationContext& context) const {
  if (!registry_) {
    return {};
  }

  std::vector<std::string> providers;
  const std::vector<std::string> allowed = normalizedAvailableModels(context);
  if (!allowed.empty()) {
    for (const std::string& provider : allowed) {
      if (registry_->hasProvider(provider)) {
        providers.push_back(provider);
      }
    }
    return providers;
  }

  providers = registry_->listProviders();
  for (std::string& provider : providers) {
    provider = normalizeProviderName(provider);
  }
  std::sort(providers.begin(), providers.end());
  providers.erase(std::unique(providers.begin(), providers.end()), providers.end());
  return providers;
}

std::vector<std::string> MultiModelOrchestrator::buildCandidateProviders(
    const OrchestrationContext& context) const {
  const std::vector<std::string> available = availableProviders(context);
  if (available.empty()) {
    return {};
  }

  std::vector<std::string> candidates;
  const std::string preferred = preferredProviderFor(context, available);
  pushUnique(candidates, preferred);

  for (const std::string& provider : available) {
    if (isLocalProvider(provider)) {
      pushUnique(candidates, provider);
    }
  }

  pushUnique(candidates, defaultProviderFor(available));
  for (const std::string& provider : available) {
    pushUnique(candidates, provider);
  }
  return candidates;
}

std::string MultiModelOrchestrator::preferredProviderFor(
    const OrchestrationContext& context,
    const std::vector<std::string>& availableProviders) const {
  const nlohmann::ordered_json routing =
      routingConfig_.value("routing", nlohmann::ordered_json::object());
  const std::string taskKey = toString(context.taskType);
  if (routing.contains(taskKey) && routing.at(taskKey).is_string()) {
    const std::string preferred =
        normalizeProviderName(routing.at(taskKey).get<std::string>());
    if (containsValue(availableProviders, preferred)) {
      return preferred;
    }
  }

  if (context.complexity == TaskComplexity::High && routing.contains("reasoning") &&
      routing.at("reasoning").is_string()) {
    const std::string reasoningProvider =
        normalizeProviderName(routing.at("reasoning").get<std::string>());
    if (containsValue(availableProviders, reasoningProvider)) {
      return reasoningProvider;
    }
  }

  if (context.latencyBudgetMs > 0U && context.latencyBudgetMs <= 250U) {
    for (const std::string& provider : availableProviders) {
      if (isLocalProvider(provider)) {
        return provider;
      }
    }
  }

  return {};
}

std::string MultiModelOrchestrator::defaultProviderFor(
    const std::vector<std::string>& availableProviders) const {
  if (availableProviders.empty()) {
    return {};
  }

  const nlohmann::ordered_json routing =
      routingConfig_.value("routing", nlohmann::ordered_json::object());
  if (routing.contains("analysis") && routing.at("analysis").is_string()) {
    const std::string analysisProvider =
        normalizeProviderName(routing.at("analysis").get<std::string>());
    if (containsValue(availableProviders, analysisProvider)) {
      return analysisProvider;
    }
  }

  return availableProviders.front();
}

bool MultiModelOrchestrator::isLocalProvider(
    const std::string& providerName) const {
  if (!registry_ || !registry_->hasProvider(providerName)) {
    return false;
  }

  std::string error;
  std::unique_ptr<model::IModelAdapter> adapter =
      registry_->create(providerName, error);
  if (!adapter) {
    return false;
  }

  const bool localProvider = adapter->modelInfo().localProvider;
  adapter->shutdown();
  return localProvider;
}

std::string MultiModelOrchestrator::normalizeProviderName(std::string value) {
  return lowerAscii(std::move(value));
}

}  // namespace ultra::ai::orchestration
