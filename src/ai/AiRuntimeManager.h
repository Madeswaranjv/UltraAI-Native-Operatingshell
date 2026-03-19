#pragma once

#include <external/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {

struct AiStatusSnapshot {
  bool runtimeActive{false};
  bool indexPresent{false};
  bool integrityOk{false};
  std::size_t filesIndexed{0};
  std::size_t symbolsIndexed{0};
  std::size_t dependenciesIndexed{0};
  std::size_t pendingChanges{0};
  std::uint32_t schemaVersion{0U};
  std::uint32_t indexVersion{0U};
};

struct MetricsAggregate {
  struct NodeCountEntry {
    std::size_t nodeCount{0U};
    std::size_t count{0U};
  };

  struct Snapshot {
    double avgCreationTimeUs{0.0};
    double maxCreationTimeUs{0.0};
    std::vector<NodeCountEntry> nodeCountDistribution;
  };

  struct Context {
    double avgCompressionTimeUs{0.0};
    double avgTokensSaved{0.0};
    double compressionRatio{0.0};
    double contextReuseRate{0.0};
    double hotSliceHitRate{0.0};
  };

  struct Branch {
    double avgChurnTimeUs{0.0};
    std::uint64_t evictionCount{0U};
    double overlayReuseRate{0.0};
  };

  struct Token {
    std::uint64_t totalTokensSaved{0U};
    double avgSavingsPercent{0.0};
    std::uint64_t estimatedLlmCallsAvoided{0U};
  };

  struct MemoryGovernance {
    std::uint64_t snapshotVersion{0U};
    std::string branchId;
    std::size_t activeOverlayCount{0U};
    std::size_t activeOverlayLimit{0U};
    std::size_t hotSliceCurrentSize{0U};
    std::size_t hotSliceTargetCapacity{0U};
    double hotSliceHitRate{0.0};
    double contextReuseRate{0.0};
    double tokenBudgetScale{1.0};
    std::size_t compressionDepth{1U};
    double pruningThreshold{0.0};
    double impactPredictionAccuracy{0.0};
    std::size_t recalibrationCount{0U};
    std::uint64_t evictions{0U};
  };

  struct ReflectiveOptimization {
    double tokenSavings{0.0};
    double contextReuseRate{0.0};
    double hotSliceHitRate{0.0};
    double impactPredictionAccuracy{0.0};
    double compressionEfficiency{0.0};
    std::size_t weightAdjustmentCount{0U};
    std::vector<std::string> weightAdjustments;
  };

  struct CpuGovernor {
    std::size_t activeWorkloads{0U};
    std::size_t workloadCount{0U};
    double avgExecutionTimeMs{0.0};
    std::size_t hardwareThreads{0U};
    std::size_t recommendedThreads{0U};
    std::size_t minRecommendedThreads{0U};
    std::size_t maxRecommendedThreads{0U};
    std::size_t calibrationCount{0U};
    bool idle{false};
    std::vector<std::string> activeWorkloadNames;
  };

  struct MetaCognitive {
    double stabilityScore{0.0};
    double driftScore{0.0};
    double learningVelocity{0.0};
    bool conservativeMode{false};
    bool exploratoryMode{false};
    std::string predictedNextCommand;
    std::size_t queryTokenBudget{0U};
    std::size_t queryCacheCapacity{0U};
    std::size_t hotSliceCapacity{0U};
    std::size_t branchRetentionHint{0U};
    std::size_t recalibrationCount{0U};
  };

  bool enabled{false};
  Snapshot snapshot;
  Context context;
  Branch branch;
  Token token;
  MemoryGovernance memoryGovernance;
  ReflectiveOptimization reflectiveOptimization;
  CpuGovernor cpuGovernor;
  MetaCognitive metaCognitive;
};

class AiRuntimeManager {
 public:
  explicit AiRuntimeManager(std::filesystem::path projectRoot);

  int wakeAi(bool verbose = true);
  int rebuildAi(bool verbose = true);
  int aiStatus(bool verbose = true);
  int aiVerify(bool verbose = true);
  bool contextDiff(nlohmann::json& payloadOut, std::string& error);
  MetricsAggregate collectMetrics() const;
  MetricsAggregate collectMetrics(const std::string& action,
                                 std::string& error) const;
  void silentIncrementalUpdate();
  static bool requestDaemon(const std::filesystem::path& projectRoot,
                            const std::string& command,
                            nlohmann::json& response,
                            std::string& error);
  static bool requestDaemon(const std::filesystem::path& projectRoot,
                            const std::string& command,
                            const nlohmann::json& requestPayload,
                            nlohmann::json& response,
                            std::string& error);

 private:
  static bool daemonChildModeEnabled();
  int runDaemonLoop(bool verbose);

  std::filesystem::path projectRoot_;
};

}  // namespace ultra::ai

