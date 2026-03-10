#include "WeightManager.h"
#include "../core/Logger.h"
#include <external/json.hpp>
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>

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
  save();
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
  nlohmann::json j;
  std::vector<std::string> keys;
  keys.reserve(weights_.size());
  for (const auto& kv : weights_) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());
  for (const auto& name : keys) {
    j[name] = weights_.at(name);
  }

  std::ofstream out(weightsFile_);
  if (!out) {
    ultra::core::Logger::error(ultra::core::LogCategory::General, 
        "Failed to write weights.json: " + weightsFile_.string());
    return false;
  }

  out << j.dump(2);
  return true;
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
