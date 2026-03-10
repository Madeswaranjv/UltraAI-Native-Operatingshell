#include "BiasAdjuster.h"
#include "../core/Logger.h"

namespace ultra::calibration {

BiasAdjuster::BiasAdjuster(WeightManager& manager) : manager_(manager) {}

void BiasAdjuster::applyPatternBias(const std::string& detectedPattern) {
  if (detectedPattern == "diff" || detectedPattern == "analyze") {
    // Boost risk and structural weights if user is heavily analyzing diffs
    float riskWeight = manager_.getWeight("risk_score_weight", 1.0f);
    manager_.setWeight("risk_score_weight", riskWeight * 1.05f);
    ultra::core::Logger::info(ultra::core::LogCategory::General, 
        "Detected analytical pattern. Boosted risk_score_weight.");
  } else if (detectedPattern == "build" || detectedPattern == "build-incremental") {
    // Boost speed/caching weights if user is repeatedly building
    float cacheWeight = manager_.getWeight("cache_retention_weight", 1.0f);
    manager_.setWeight("cache_retention_weight", cacheWeight * 1.05f);
    ultra::core::Logger::info(ultra::core::LogCategory::General, 
        "Detected repetitive build pattern. Boosted cache_retention_weight.");
  }
}

}  // namespace ultra::calibration
