#include "CPUGovernor.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace ultra::runtime {

namespace {

constexpr double kMovingAverageAlpha = 0.2;
constexpr std::size_t kCalibrationInterval = 4U;
constexpr std::size_t kFallbackHardwareThreads = 4U;

}  // namespace

CPUGovernor& CPUGovernor::instance() {
  static CPUGovernor governor;
  return governor;
}

std::size_t CPUGovernor::sanitizeThreadLimit(const std::size_t maxThreads) {
  return std::max<std::size_t>(1U, maxThreads);
}

double CPUGovernor::sanitizeMs(const double ms) {
  if (!std::isfinite(ms) || ms < 0.0) {
    return 0.0;
  }
  return ms;
}

double CPUGovernor::workloadBaseWeight(const std::string& name) {
  if (name.find("scan") != std::string::npos ||
      name.find("impact") != std::string::npos) {
    return 0.58;
  }
  if (name.find("compression") != std::string::npos ||
      name.find("context") != std::string::npos) {
    return 0.72;
  }
  if (name.find("incremental") != std::string::npos ||
      name.find("query") != std::string::npos) {
    return 0.88;
  }
  return 0.80;
}

double CPUGovernor::blendAverage(const double previous, const double sample) {
  return previous <= 0.0
             ? sample
             : ((1.0 - kMovingAverageAlpha) * previous) +
                   (kMovingAverageAlpha * sample);
}

std::size_t CPUGovernor::detectedHardwareThreads() const noexcept {
  const unsigned int detected = std::thread::hardware_concurrency();
  return detected == 0U ? kFallbackHardwareThreads
                        : static_cast<std::size_t>(detected);
}

std::string CPUGovernor::normalizeWorkloadName(const std::string& name) const {
  if (name.empty()) {
    return "default";
  }
  return name;
}

std::string CPUGovernor::preferredWorkloadLocked(
    const std::string& requestedName) const {
  const std::string normalized = normalizeWorkloadName(requestedName);
  if (requestedName.size() != 0U || workloads_.empty()) {
    return normalized;
  }

  std::string selected = normalized;
  std::size_t bestActive = 0U;
  for (const auto& [name, stats] : workloads_) {
    if (stats.activeCount == 0U) {
      continue;
    }
    if (stats.activeCount > bestActive ||
        (stats.activeCount == bestActive && (selected == normalized || name < selected))) {
      selected = name;
      bestActive = stats.activeCount;
    }
  }
  if (bestActive != 0U) {
    return selected;
  }

  for (const auto& [name, stats] : workloads_) {
    if (stats.registrationCount == 0U) {
      continue;
    }
    return name;
  }

  return normalized;
}

void CPUGovernor::calibrateLocked() {
  hardwareThreads_ = detectedHardwareThreads();
  minRecommendedThreads_ = std::max<std::size_t>(1U, hardwareThreads_ / 4U);
  maxRecommendedThreads_ = std::max<std::size_t>(1U, hardwareThreads_);

  if (idle_) {
    maxRecommendedThreads_ =
        std::max<std::size_t>(1U, std::min(maxRecommendedThreads_,
                                           hardwareThreads_ / 2U));
  }

  if (movingAverageMs_ > 0.0) {
    lightTaskThresholdMs_ =
        std::clamp(movingAverageMs_ * 0.75, 8.0, 32.0);
    heavyTaskThresholdMs_ =
        std::clamp(movingAverageMs_ * 2.5, 48.0, 240.0);
  } else {
    lightTaskThresholdMs_ = 20.0;
    heavyTaskThresholdMs_ = 120.0;
  }

  for (auto& [name, stats] : workloads_) {
    const double baseWeight = workloadBaseWeight(name);
    double weight = baseWeight;
    if (stats.averageExecutionMs >= heavyTaskThresholdMs_) {
      weight -= 0.20;
    } else if (stats.averageExecutionMs >= lightTaskThresholdMs_) {
      weight -= 0.10;
    } else {
      weight += 0.12;
    }
    if (stats.activeCount > 1U) {
      weight -= 0.08;
    }

    const std::size_t span =
        maxRecommendedThreads_ >= minRecommendedThreads_
            ? (maxRecommendedThreads_ - minRecommendedThreads_)
            : 0U;
    const std::size_t recommendation = minRecommendedThreads_ +
        static_cast<std::size_t>(std::llround(
            std::clamp(weight, 0.25, 1.0) * static_cast<double>(span)));
    stats.recommendedThreadCount = std::clamp(recommendation,
                                              minRecommendedThreads_,
                                              maxRecommendedThreads_);
  }

  ++calibrationCount_;
}

void CPUGovernor::maybeCalibrateLocked() {
  ++calibrationCounter_;
  if (calibrationCounter_ == 1U ||
      (calibrationCounter_ % kCalibrationInterval) == 0U) {
    calibrateLocked();
  }
}

void CPUGovernor::registerWorkload(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string workloadName = normalizeWorkloadName(name);
  GovernorWorkloadStats& stats = workloads_[workloadName];
  ++stats.activeCount;
  ++stats.registrationCount;
  ++activeWorkloads_;
  idle_ = false;
  maybeCalibrateLocked();
}

void CPUGovernor::recordExecutionTime(const std::string& name, const double ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string workloadName = normalizeWorkloadName(name);
  const double sample = sanitizeMs(ms);
  GovernorWorkloadStats& stats = workloads_[workloadName];

  stats.averageExecutionMs =
      blendAverage(stats.averageExecutionMs, sample);
  ++stats.sampleCount;
  if (stats.activeCount > 0U) {
    --stats.activeCount;
  }

  movingAverageMs_ = blendAverage(movingAverageMs_, sample);
  if (activeWorkloads_ > 0U) {
    --activeWorkloads_;
  }
  maybeCalibrateLocked();
}

std::size_t CPUGovernor::recommendedThreadCount() const {
  return recommendedThreadCount(detectedHardwareThreads());
}

std::size_t CPUGovernor::recommendedThreadCount(
    const std::size_t maxThreads) const {
  return recommendedThreadCount(maxThreads, {});
}

std::size_t CPUGovernor::recommendedThreadCount(
    const std::size_t maxThreads,
    const std::string& workloadName) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const_cast<CPUGovernor*>(this)->maybeCalibrateLocked();
  return recommendedThreadCountLocked(sanitizeThreadLimit(maxThreads), workloadName);
}

std::size_t CPUGovernor::recommendedThreadCountLocked(
    const std::size_t maxThreads,
    const std::string& workloadName) const {
  const std::size_t effectiveHardware =
      std::max<std::size_t>(1U, std::min(maxThreads, hardwareThreads_));
  const std::size_t minThreads =
      std::max<std::size_t>(1U, std::min(minRecommendedThreads_, effectiveHardware));
  const std::size_t maxAllowedThreads =
      std::max(minThreads,
               std::min(maxRecommendedThreads_, effectiveHardware));

  const std::string preferredWorkload = preferredWorkloadLocked(workloadName);
  const auto workloadIt = workloads_.find(preferredWorkload);
  if (workloadIt == workloads_.end() && activeWorkloads_ == 0U &&
      workloadName.empty()) {
    return 1U;
  }

  double workloadAverage = movingAverageMs_;
  std::size_t workloadActive = activeWorkloads_;
  double baseWeight = workloadBaseWeight(preferredWorkload);
  std::size_t calibratedThreads = maxAllowedThreads;
  if (workloadIt != workloads_.end()) {
    workloadAverage = workloadIt->second.averageExecutionMs > 0.0
                          ? workloadIt->second.averageExecutionMs
                          : workloadAverage;
    workloadActive = workloadIt->second.activeCount > 0U
                         ? workloadIt->second.activeCount
                         : workloadActive;
    calibratedThreads = std::clamp(workloadIt->second.recommendedThreadCount,
                                   minThreads, maxAllowedThreads);
  }

  double loadPenalty = 0.0;
  if (workloadAverage >= heavyTaskThresholdMs_) {
    loadPenalty += 0.25;
  } else if (workloadAverage >= lightTaskThresholdMs_) {
    loadPenalty += 0.12;
  } else {
    baseWeight += 0.10;
  }

  if (activeWorkloads_ > 1U && effectiveHardware > 1U) {
    loadPenalty += std::min(
        0.30,
        static_cast<double>(activeWorkloads_ - 1U) /
            static_cast<double>(effectiveHardware));
  }
  if (workloadActive > 1U) {
    loadPenalty += 0.08;
  }
  if (idle_) {
    loadPenalty += 0.20;
  }

  const std::size_t span = maxAllowedThreads - minThreads;
  const std::size_t weightedThreads = minThreads +
      static_cast<std::size_t>(std::llround(
          std::clamp(baseWeight - loadPenalty, 0.20, 1.0) *
          static_cast<double>(span)));
  return std::clamp(std::min(calibratedThreads, weightedThreads),
                    minThreads, maxAllowedThreads);
}

void CPUGovernor::enterIdle() {
  std::lock_guard<std::mutex> lock(mutex_);
  idle_ = true;
  maybeCalibrateLocked();
  runOverlayCleanupCheck();
  runRetireReclamationCheck();
  runHotSliceShrinkCheck();
}

void CPUGovernor::exitIdle() {
  std::lock_guard<std::mutex> lock(mutex_);
  idle_ = false;
  maybeCalibrateLocked();
}

GovernorStats CPUGovernor::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const_cast<CPUGovernor*>(this)->maybeCalibrateLocked();
  GovernorStats out;
  out.activeWorkloads = activeWorkloads_;
  out.calibrationCount = calibrationCount_;
  out.hardwareThreads = hardwareThreads_;
  out.idle = idle_;
  out.minRecommendedThreadCount = minRecommendedThreads_;
  out.maxRecommendedThreadCount = maxRecommendedThreads_;
  out.movingAverageMs = movingAverageMs_;
  out.recommendedThreadCount =
      recommendedThreadCountLocked(hardwareThreads_, {});
  out.workloads = workloads_;
  for (auto& [name, stats] : out.workloads) {
    stats.recommendedThreadCount =
        recommendedThreadCountLocked(hardwareThreads_, name);
  }
  return out;
}

void CPUGovernor::runOverlayCleanupCheck() {
  ++overlayCleanupTicks_;
}

void CPUGovernor::runRetireReclamationCheck() {
  ++retireCheckTicks_;
}

void CPUGovernor::runHotSliceShrinkCheck() {
  ++hotSliceShrinkTicks_;
}

}  // namespace ultra::runtime
