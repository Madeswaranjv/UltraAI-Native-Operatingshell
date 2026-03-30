#include "WeightManager.h"
#include "../core/Logger.h"
#include <external/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace {

constexpr int kMaxWriteAttempts = 2;

struct PersistenceState {
  std::mutex mutex;
  bool workerRunning{false};
  bool persistenceDisabled{false};
  bool failureLogged{false};
  std::optional<nlohmann::json> pendingPayload;
};

nlohmann::json buildSortedPayload(
    const std::unordered_map<std::string, float>& weights) {
  nlohmann::json payload;
  std::vector<std::string> keys;
  keys.reserve(weights.size());
  for (const auto& kv : weights) {
    keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end());
  for (const auto& name : keys) {
    payload[name] = weights.at(name);
  }
  return payload;
}

std::shared_ptr<PersistenceState> persistenceStateForPath(
    const std::filesystem::path& path) {
  static std::mutex registryMutex;
  static std::unordered_map<std::string, std::weak_ptr<PersistenceState>>
      registry;

  const std::string key = path.lexically_normal().string();
  std::lock_guard<std::mutex> lock(registryMutex);
  const auto found = registry.find(key);
  if (found != registry.end()) {
    if (std::shared_ptr<PersistenceState> existing = found->second.lock()) {
      return existing;
    }
  }

  auto created = std::make_shared<PersistenceState>();
  registry[key] = created;
  return created;
}

bool writePayloadWithRetry(const std::filesystem::path& weightsFile,
                           const nlohmann::json& payload) {
  for (int attempt = 0; attempt < kMaxWriteAttempts; ++attempt) {
    std::error_code ec;
    std::filesystem::create_directories(weightsFile.parent_path(), ec);
    if (ec) {
      return false;
    }

    std::ofstream out(weightsFile, std::ios::binary | std::ios::trunc);
    if (!out) {
      continue;
    }

    out << payload.dump(2);
    out.flush();
    if (out.good()) {
      return true;
    }
  }
  return false;
}

void runPersistenceWorker(const std::filesystem::path& weightsFile,
                          const std::shared_ptr<PersistenceState>& state) {
  while (true) {
    std::optional<nlohmann::json> pendingPayload;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->persistenceDisabled) {
        state->pendingPayload.reset();
        state->workerRunning = false;
        return;
      }

      if (!state->pendingPayload.has_value()) {
        state->workerRunning = false;
        return;
      }

      pendingPayload = std::move(state->pendingPayload);
      state->pendingPayload.reset();
    }

    if (pendingPayload.has_value() &&
        writePayloadWithRetry(weightsFile, *pendingPayload)) {
      continue;
    }

    bool shouldLog = false;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->persistenceDisabled = true;
      state->pendingPayload.reset();
      state->workerRunning = false;
      if (!state->failureLogged) {
        shouldLog = true;
        state->failureLogged = true;
      }
    }

    if (shouldLog) {
      ultra::core::Logger::error(
          ultra::core::LogCategory::General,
          "Failed to write weights.json after " +
              std::to_string(kMaxWriteAttempts) + " attempts: " +
              weightsFile.string() +
              " (disabling calibration persistence for this process)");
    }
    return;
  }
}

void enqueueNonBlockingSave(const std::filesystem::path& weightsFile,
                            nlohmann::json payload) {
  const std::shared_ptr<PersistenceState> state =
      persistenceStateForPath(weightsFile);

  bool startWorker = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->persistenceDisabled) {
      return;
    }

    state->pendingPayload = std::move(payload);
    if (!state->workerRunning) {
      state->workerRunning = true;
      startWorker = true;
    }
  }

  if (startWorker) {
    std::thread([weightsFile, state]() {
      runPersistenceWorker(weightsFile, state);
    }).detach();
  }
}

}  // namespace

namespace ultra::calibration {

WeightManager::WeightManager(const std::filesystem::path& baseDir) {
  auto dir = baseDir / "calibration";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  weightsFile_ = dir / "weights.json";
  load();
}

float WeightManager::getWeight(const std::string& name, float defaultVal) const {
  auto it = weights_.find(name);
  if (it != weights_.end()) {
    return it->second;
  }
  return defaultVal;
}

void WeightManager::setWeight(const std::string& name, float value) {
  weights_[name] = value;
  enqueueNonBlockingSave(weightsFile_, buildSortedPayload(weights_));
}

bool WeightManager::load() {
  std::error_code ec;
  if (!std::filesystem::exists(weightsFile_, ec)) {
    return false;
  }

  std::ifstream in(weightsFile_);
  if (!in) return false;

  try {
    nlohmann::json j;
    in >> j;
    
    weights_.clear();
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.value().is_number()) {
        weights_[it.key()] = it.value().get<float>();
      }
    }
    return true;
  } catch (const std::exception& e) {
    ultra::core::Logger::error(ultra::core::LogCategory::General, 
        "Failed to load weights.json: " + std::string(e.what()));
    return false;
  }
}

bool WeightManager::save() const {
  const nlohmann::json payload = buildSortedPayload(weights_);
  if (writePayloadWithRetry(weightsFile_, payload)) {
    return true;
  }

  ultra::core::Logger::error(
      ultra::core::LogCategory::General,
      "Failed to write weights.json after " +
          std::to_string(kMaxWriteAttempts) + " attempts: " +
          weightsFile_.string());
  return false;
}

void WeightManager::reset() {
  weights_.clear();
  std::error_code ec;
  std::filesystem::remove(weightsFile_, ec);
}

std::unordered_map<std::string, float> WeightManager::getAllWeights() const {
  return weights_;
}

}  // namespace ultra::calibration

