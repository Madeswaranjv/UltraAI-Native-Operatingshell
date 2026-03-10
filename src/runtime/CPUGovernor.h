#pragma once

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

namespace ultra::runtime {

struct GovernorWorkloadStats {
  std::size_t activeCount{0U};
  std::size_t registrationCount{0U};
  std::size_t sampleCount{0U};
  double averageExecutionMs{0.0};
  std::size_t recommendedThreadCount{1U};
};

struct GovernorStats {
  std::size_t activeWorkloads{0U};
  std::size_t calibrationCount{0U};
  std::size_t hardwareThreads{1U};
  std::size_t recommendedThreadCount{1U};
  std::size_t minRecommendedThreadCount{1U};
  std::size_t maxRecommendedThreadCount{1U};
  double movingAverageMs{0.0};
  bool idle{false};
  std::map<std::string, GovernorWorkloadStats> workloads;
};

class CPUGovernor {
 public:
  static CPUGovernor& instance();

  void registerWorkload(const std::string& name);
  void recordExecutionTime(const std::string& name, double ms);

  [[nodiscard]] std::size_t recommendedThreadCount() const;
  [[nodiscard]] std::size_t recommendedThreadCount(
      std::size_t maxThreads) const;
  [[nodiscard]] std::size_t recommendedThreadCount(
      std::size_t maxThreads,
      const std::string& workloadName) const;

  void enterIdle();
  void exitIdle();

  [[nodiscard]] GovernorStats stats() const;

 private:
  CPUGovernor() = default;

  [[nodiscard]] static std::size_t sanitizeThreadLimit(std::size_t maxThreads);
  [[nodiscard]] static double sanitizeMs(double ms);
  [[nodiscard]] static double workloadBaseWeight(const std::string& name);
  [[nodiscard]] static double blendAverage(double previous, double sample);
  [[nodiscard]] std::size_t detectedHardwareThreads() const noexcept;
  [[nodiscard]] std::string normalizeWorkloadName(const std::string& name) const;
  [[nodiscard]] std::string preferredWorkloadLocked(
      const std::string& requestedName) const;
  void calibrateLocked();
  void maybeCalibrateLocked();
  [[nodiscard]] std::size_t recommendedThreadCountLocked(
      std::size_t maxThreads,
      const std::string& workloadName) const;
  void runOverlayCleanupCheck();
  void runRetireReclamationCheck();
  void runHotSliceShrinkCheck();

  mutable std::mutex mutex_;
  std::size_t activeWorkloads_{0U};
  double movingAverageMs_{0.0};
  bool idle_{false};
  std::size_t overlayCleanupTicks_{0U};
  std::size_t retireCheckTicks_{0U};
  std::size_t hotSliceShrinkTicks_{0U};
  std::size_t calibrationCounter_{0U};
  std::size_t calibrationCount_{0U};
  std::size_t hardwareThreads_{1U};
  std::size_t minRecommendedThreads_{1U};
  std::size_t maxRecommendedThreads_{1U};
  double lightTaskThresholdMs_{20.0};
  double heavyTaskThresholdMs_{120.0};
  std::map<std::string, GovernorWorkloadStats> workloads_;
};

}  // namespace ultra::runtime
