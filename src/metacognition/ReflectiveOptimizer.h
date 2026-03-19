#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ultra::metacognition {

class ReflectiveOptimizer {
 public:
  static ReflectiveOptimizer& instance();

  [[nodiscard]] double tokenSavings() const;
  [[nodiscard]] double contextReuseRate() const;
  [[nodiscard]] double hotSliceHitRate() const;
  [[nodiscard]] double impactPredictionAccuracy() const;
  [[nodiscard]] double compressionEfficiency() const;
  [[nodiscard]] std::size_t weightAdjustmentCount() const;
  [[nodiscard]] std::vector<std::string> weightAdjustments() const;

 private:
  ReflectiveOptimizer() = default;
};

}  // namespace ultra::metacognition
