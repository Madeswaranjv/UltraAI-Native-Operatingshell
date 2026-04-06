#include "WeightManager.h"
#include "../core/Logger.h"
#include <external/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <vector>

namespace {

constexpr int kMaxWriteAttempts = 2;

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
  (void)save();
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

