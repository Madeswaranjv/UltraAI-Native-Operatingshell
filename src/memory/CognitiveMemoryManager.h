#pragma once

#include "../calibration/WeightManager.h"
#include "../calibration/WeightTuner.h"
#include "../runtime/CognitiveState.h"
#include "../runtime/GraphSnapshot.h"
#include "EpisodicMemory.h"
#include "HotSlice.h"
#include "SemanticMemory.h"
#include "StrategicMemory.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <shared_mutex>
#include <string>

namespace ultra::memory {

class CognitiveMemoryManager {
 public:
  struct MemoryGovernanceState {
    std::uint64_t snapshotVersion{0U};
    std::string branchId;
    std::size_t activeOverlayCount{0U};
    std::size_t activeOverlayLimit{3U};
    std::size_t targetHotSliceCapacity{HotSlice::kMaxHotSliceEntries};
    double hotSliceCapacityScale{1.0};
    double tokenBudgetScale{1.0};
    std::size_t compressionDepth{1U};
    double pruningThreshold{0.10};
    double hotSliceHitRate{0.0};
    double contextReuseRate{0.0};
    double compressionRatio{1.0};
    double impactPredictionAccuracy{0.0};
    std::size_t governancePassCount{0U};
    std::size_t weightAdjustmentCount{0U};
  };

  struct IdentityState {
    std::uint32_t schemaVersion{1U};
    std::string instanceId;
    std::uint64_t deterministicSeed{0U};
  };

  explicit CognitiveMemoryManager(
      const std::filesystem::path& projectRoot = std::filesystem::current_path());

  void bindToSnapshot(const runtime::GraphSnapshot* snapshot);

  /// Call once after each index rebuild with the live graph dimensions.
  /// Immediately resizes working HotSlice to the graph-proportional capacity.
  void setGraphScale(std::size_t graphNodeCount,
                     std::size_t avgSnapshotNodes = 0U) noexcept;

  void recordIntentExecution(const std::string& actionId,
                             const runtime::GraphSnapshot& snapshot,
                             bool success,
                             bool rolledBack,
                             const std::string& message);
  void recordIntentStart(const std::string& intentId,
                         const runtime::GraphSnapshot& snapshot,
                         double riskScore,
                         double confidenceScore,
                         const std::string& message);

  void recordIntentEvaluation(const std::string& intentId,
                              const runtime::GraphSnapshot& snapshot,
                              double rankedScore,
                              double riskScore,
                              double confidenceScore,
                              const std::string& message);

  void recordRiskEvaluation(const std::string& subject,
                            const runtime::GraphSnapshot& snapshot,
                            double predictedRiskScore,
                            double observedRiskScore,
                            double confidenceScore,
                            const std::string& message);

  void recordMergeOutcome(const std::string& mergeTarget,
                          const runtime::GraphSnapshot& snapshot,
                          bool success,
                          const std::string& message);

  void updateSemanticEvolution(const std::string& nodeId,
                               const std::string& symbolName,
                               const std::string& signature,
                               const std::string& changeType,
                               std::uint64_t version,
                               const std::string& predecessorNodeId = {});

  void applyMemoryGovernance(std::size_t activeOverlayCount,
                             bool heavyMutation);
  void applyStrategicAdjustments(
      const PerformanceSnapshot* snapshot = nullptr);
  [[nodiscard]] std::size_t governedTokenBudget(
      std::size_t requestedBudget) const;
  [[nodiscard]] std::size_t governedHotSliceCapacity(
      std::size_t requestedCapacity) const;
  [[nodiscard]] runtime::RelevanceProfile resolvedRelevanceProfile(
      const runtime::RelevanceProfile& requested = {}) const;
  [[nodiscard]] const MemoryGovernanceState& governanceState() const noexcept {
    return governanceState_;
  }

  [[nodiscard]] const IdentityState& identity() const noexcept {
    return identity_;
  }

  HotSlice working;
  EpisodicMemory episodic;
  SemanticMemory semantic;
  StrategicMemory strategic;

 private:
  static constexpr std::uint32_t kSchemaVersion = 1U;

  static std::uint64_t deterministicSeedForPath(
      const std::filesystem::path& path);
  static std::string buildInstanceId(std::uint64_t deterministicSeed);
  static runtime::RelevanceProfile normalizeRelevanceProfile(
      runtime::RelevanceProfile profile);

  void ensureStorageDirectories();
  void loadIdentity();
  void persistIdentity() const;
  void loadMemory();
  void persistEpisodic() const;
  void persistSemantic() const;
  void persistStrategic() const;
  void seedDefaultWeights();
  void rebuildResolvedProfile();

  std::filesystem::path projectRoot_;
  std::filesystem::path storageDir_;
  std::filesystem::path episodicPath_;
  std::filesystem::path semanticPath_;
  std::filesystem::path strategicPath_;
  std::filesystem::path metadataPath_;


  std::uint64_t boundVersion_{0U};
  runtime::BranchId boundBranch_{runtime::BranchId::nil()};
  calibration::WeightManager weightManager_;
  calibration::WeightTuner weightTuner_;
  IdentityState identity_;
  MemoryGovernanceState governanceState_;
  runtime::RelevanceProfile optimizedProfile_{};
  std::size_t graphNodeCount_{0U};
  std::size_t avgSnapshotNodes_{0U};
};

}  // namespace ultra::memory
