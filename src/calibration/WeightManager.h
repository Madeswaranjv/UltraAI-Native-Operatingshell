#pragma once

#include <string>
#include <unordered_map>
#include <filesystem>

namespace ultra::calibration {

/// Manages a collection of tunable weights for scoring algorithms.
class WeightManager {
 public:
  explicit WeightManager(const std::filesystem::path& baseDir);
  
  /// Get the current float value for a weight, or the default if undefined.
  float getWeight(const std::string& name, float defaultVal = 1.0f) const;
  
  /// Set a weight and save the configuration to disk.
  void setWeight(const std::string& name, float value);
  
  /// Reload weights from disk.
  bool load();
  
  /// Save all weights to disk.
  bool save() const;

  /// Clear all weights and reset to factory defaults on disk.
  void reset();

  /// Retrieve all current weights.
  std::unordered_map<std::string, float> getAllWeights() const;

 private:
  std::filesystem::path weightsFile_;
  std::unordered_map<std::string, float> weights_;
};

}  // namespace ultra::calibration
