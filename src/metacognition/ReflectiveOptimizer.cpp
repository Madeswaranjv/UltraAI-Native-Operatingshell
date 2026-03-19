#include "ReflectiveOptimizer.h"

#include "MetaCognitiveOrchestrator.h"
#include "../metrics/PerformanceMetrics.h"

namespace ultra::metacognition {

ReflectiveOptimizer& ReflectiveOptimizer::instance() {
  static ReflectiveOptimizer optimizer;
  return optimizer;
}

double ReflectiveOptimizer::tokenSavings() const {
  return metrics::PerformanceMetrics::averageTokenSavingsRatio();
}

double ReflectiveOptimizer::contextReuseRate() const {
  return metrics::PerformanceMetrics::instance().contextReuseRate();
}

double ReflectiveOptimizer::hotSliceHitRate() const {
  return metrics::PerformanceMetrics::instance().hotSliceHitRate();
}

double ReflectiveOptimizer::impactPredictionAccuracy() const {
  const double sampled =
      metrics::PerformanceMetrics::instance().impactPredictionAccuracy();
  if (sampled > 0.0) {
    return sampled;
  }
  return MetaCognitiveOrchestrator::instance().stabilityScore();
}

double ReflectiveOptimizer::compressionEfficiency() const {
  return tokenSavings();
}

std::size_t ReflectiveOptimizer::weightAdjustmentCount() const {
  return metrics::PerformanceMetrics::instance().weightAdjustmentCount();
}

std::vector<std::string> ReflectiveOptimizer::weightAdjustments() const {
  return metrics::PerformanceMetrics::instance().weightAdjustmentNames();
}

}  // namespace ultra::metacognition
