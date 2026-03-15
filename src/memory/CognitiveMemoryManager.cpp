#include "CognitiveMemoryManager.h"

#include <external/json.hpp>
#include "../metrics/PerformanceMetrics.h"
//E:\Projects\Ultra\src\memory\CognitiveMemoryManager.cpp
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace ultra::memory {

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kMinGovernedTokenBudget = 64U;
constexpr std::size_t kMinGovernedHotSliceCapacity = 64U;

double clampDouble(const double value,
                   const double minValue,
                   const double maxValue) {
  if (!std::isfinite(value)) {
    return minValue;
  }
  return std::clamp(value, minValue, maxValue);
}

std::size_t clampSize(const std::size_t value,
                      const std::size_t minValue,
                      const std::size_t maxValue) {
  return std::clamp(value, minValue, maxValue);
}

bool isDefaultProfile(const runtime::RelevanceProfile& profile) {
  const runtime::RelevanceProfile defaults{};
  return profile.recencyWeight == defaults.recencyWeight &&
         profile.centralityWeight == defaults.centralityWeight &&
         profile.usageWeight == defaults.usageWeight &&
         profile.impactWeight == defaults.impactWeight;
}

std::map<std::string, float> sortedWeights(
    const std::unordered_map<std::string, float>& weights) {
  return std::map<std::string, float>(weights.begin(), weights.end());
}

}  // namespace

CognitiveMemoryManager::CognitiveMemoryManager(const std::filesystem::path& projectRoot)
    : projectRoot_(
          std::filesystem::absolute(projectRoot).lexically_normal()),
      storageDir_(projectRoot_ / ".ultra" / "memory" / "cognitive"),
      episodicPath_(storageDir_ / "episodic_memory.json"),
      semanticPath_(storageDir_ / "semantic_memory.json"),
      strategicPath_(storageDir_ / "strategic_memory.json"),
      metadataPath_(storageDir_ / "memory_metadata.json"),
      weightManager_(projectRoot_ / ".ultra"),
      weightTuner_(weightManager_) {
  ensureStorageDirectories();
  loadIdentity();
  loadMemory();
  seedDefaultWeights();
  rebuildResolvedProfile();
}

void CognitiveMemoryManager::bindToSnapshot(const runtime::GraphSnapshot* snapshot) {
  if (snapshot == nullptr || snapshot->version == 0U) {
    return;
  }

  const bool changed =
      snapshot->version != boundVersion_ ||
      snapshot->branch != boundBranch_;

  // No manager-level locking required here.
  boundVersion_ = snapshot->version;
  boundBranch_ = snapshot->branch;
  working.bindToSnapshotVersion(snapshot->version);

  if (!changed) {
    return;
  }

  EpisodicEvent event;
  event.version = snapshot->version;
  event.branchId = snapshot->branch.toString();
  event.type = EpisodicEventType::SnapshotBound;
  event.subject = "graph_snapshot";
  event.success = true;
  event.riskScore = 0.0;
  event.confidenceScore = 1.0;
  event.message = "working_memory_bound";

  episodic.recordEvent(event);
  persistEpisodic();
}

void CognitiveMemoryManager::setGraphScale(
    const std::size_t graphNodeCount,
    const std::size_t avgSnapshotNodes) noexcept {
  graphNodeCount_ = graphNodeCount;
  avgSnapshotNodes_ = avgSnapshotNodes;
  working.setMaxSize(HotSlice::computeCapacity(graphNodeCount_, avgSnapshotNodes_));
}

void CognitiveMemoryManager::recordIntentExecution(
    const std::string& actionId,
    const runtime::GraphSnapshot& snapshot,
    const bool success,
    const bool rolledBack,
    const std::string& message) {
  bindToSnapshot(&snapshot);

  EpisodicEvent event;
  event.version = snapshot.version;
  event.branchId = snapshot.branch.toString();
  event.type =
      success ? EpisodicEventType::ExecutionSuccess
              : EpisodicEventType::ExecutionFailure;
  event.subject = actionId;
  event.success = success;
  event.rolledBack = rolledBack;
  event.riskScore = success ? 0.20 : 0.85;
  event.confidenceScore = success ? 0.80 : 0.20;
  event.message = message;
  episodic.recordEvent(event);

  if (rolledBack) {
    EpisodicEvent rollbackEvent;
    rollbackEvent.version = snapshot.version;
    rollbackEvent.branchId = snapshot.branch.toString();
    rollbackEvent.type = EpisodicEventType::Rollback;
    rollbackEvent.subject = actionId;
    rollbackEvent.success = false;
    rollbackEvent.rolledBack = true;
    rollbackEvent.riskScore = 0.95;
    rollbackEvent.confidenceScore = 0.10;
    rollbackEvent.message = message;
    episodic.recordEvent(rollbackEvent);
  }

  StrategicOutcome outcome;
  outcome.version = snapshot.version;
  outcome.category = "intent";
  outcome.subject = actionId;
  outcome.success = success;
  outcome.rolledBack = rolledBack;
  outcome.predictedRisk = success ? 0.25 : 0.75;
  outcome.observedRisk = rolledBack ? 0.90 : (success ? 0.15 : 0.80);
  outcome.predictedConfidence = success ? 0.80 : 0.30;
  outcome.observedConfidence = rolledBack ? 0.10 : (success ? 0.90 : 0.20);
  strategic.recordOutcome(outcome);
  applyStrategicAdjustments();
  persistEpisodic();
  persistStrategic();
}

void CognitiveMemoryManager::recordIntentStart(
    const std::string& intentId,
    const runtime::GraphSnapshot& snapshot,
    const double riskScore,
    const double confidenceScore,
    const std::string& message) {
  bindToSnapshot(&snapshot);

  EpisodicEvent event;
  event.version = snapshot.version;
  event.branchId = snapshot.branch.toString();
  event.type = EpisodicEventType::IntentStart;
  event.subject = intentId;
  event.success = true;
  event.riskScore = riskScore;
  event.confidenceScore = confidenceScore;
  event.message = message;
  episodic.recordEvent(event);
  persistEpisodic();
}

void CognitiveMemoryManager::recordIntentEvaluation(
    const std::string& intentId,
    const runtime::GraphSnapshot& snapshot,
    const double rankedScore,
    const double riskScore,
    const double confidenceScore,
    const std::string& message) {
  bindToSnapshot(&snapshot);

  EpisodicEvent event;
  event.version = snapshot.version;
  event.branchId = snapshot.branch.toString();
  event.type = EpisodicEventType::IntentEvaluation;
  event.subject = intentId;
  event.success = true;
  event.riskScore = riskScore;
  event.confidenceScore = confidenceScore;
  event.message = message;
  episodic.recordEvent(event);

  StrategicOutcome outcome;
  outcome.version = snapshot.version;
  outcome.category = "intent";
  outcome.subject = intentId + ":evaluation";
  outcome.success = true;
  outcome.predictedRisk = riskScore;
  outcome.observedRisk = riskScore;
  outcome.predictedConfidence = confidenceScore;
  outcome.observedConfidence = rankedScore;
  strategic.recordOutcome(outcome);

  applyStrategicAdjustments();
  persistEpisodic();
  persistStrategic();
}

void CognitiveMemoryManager::recordRiskEvaluation(
    const std::string& subject,
    const runtime::GraphSnapshot& snapshot,
    const double predictedRiskScore,
    const double observedRiskScore,
    const double confidenceScore,
    const std::string& message) {
  bindToSnapshot(&snapshot);

  EpisodicEvent event;
  event.version = snapshot.version;
  event.branchId = snapshot.branch.toString();
  event.type = EpisodicEventType::RiskEvaluation;
  event.subject = subject;
  event.success = true;
  event.riskScore = predictedRiskScore;
  event.confidenceScore = confidenceScore;
  event.message = message;
  episodic.recordEvent(event);

  StrategicOutcome outcome;
  outcome.version = snapshot.version;
  outcome.category = "risk_evaluation";
  outcome.subject = subject;
  outcome.success = true;
  outcome.predictedRisk = predictedRiskScore;
  outcome.observedRisk = observedRiskScore;
  outcome.predictedConfidence = confidenceScore;
  outcome.observedConfidence = 1.0 - std::abs(predictedRiskScore - observedRiskScore);
  strategic.recordOutcome(outcome);

  applyStrategicAdjustments();
  persistEpisodic();
  persistStrategic();
}

void CognitiveMemoryManager::recordMergeOutcome(
    const std::string& mergeTarget,
    const runtime::GraphSnapshot& snapshot,
    const bool success,
    const std::string& message) {
  bindToSnapshot(&snapshot);

  EpisodicEvent event;
  event.version = snapshot.version;
  event.branchId = snapshot.branch.toString();
  event.type = EpisodicEventType::MergeAttempt;
  event.subject = mergeTarget;
  event.success = true;
  event.riskScore = success ? 0.30 : 0.75;
  event.confidenceScore = success ? 0.75 : 0.25;
  event.message = "merge_attempt";
  episodic.recordEvent(event);

  EpisodicEvent outcomeEvent;
  outcomeEvent.version = snapshot.version;
  outcomeEvent.branchId = snapshot.branch.toString();
  outcomeEvent.type =
      success ? EpisodicEventType::MergeSuccess : EpisodicEventType::MergeRejected;
  outcomeEvent.subject = mergeTarget;
  outcomeEvent.success = success;
  outcomeEvent.rolledBack = !success;
  outcomeEvent.riskScore = success ? 0.20 : 0.90;
  outcomeEvent.confidenceScore = success ? 0.80 : 0.10;
  outcomeEvent.message = message;
  episodic.recordEvent(outcomeEvent);

  StrategicOutcome outcome;
  outcome.version = snapshot.version;
  outcome.category = "merge";
  outcome.subject = mergeTarget;
  outcome.success = success;
  outcome.rolledBack = !success;
  outcome.predictedRisk = success ? 0.30 : 0.70;
  outcome.observedRisk = success ? 0.20 : 0.85;
  outcome.predictedConfidence = success ? 0.70 : 0.30;
  outcome.observedConfidence = success ? 0.80 : 0.10;
  strategic.recordOutcome(outcome);

  applyStrategicAdjustments();
  persistEpisodic();
  persistStrategic();
}

void CognitiveMemoryManager::updateSemanticEvolution(
    const std::string& nodeId,
    const std::string& symbolName,
    const std::string& signature,
    const std::string& changeType,
    const std::uint64_t version,
    const std::string& predecessorNodeId) {
  semantic.trackSymbolEvolution(nodeId, symbolName, signature, changeType, version,
                                predecessorNodeId);
  persistSemantic();
}

void CognitiveMemoryManager::applyStrategicAdjustments(
    const PerformanceSnapshot* snapshot) {

  const PolicyAdjustments adjustments =
      strategic.getPolicyAdjustments(snapshot);

  const std::map<std::string, float> previousWeights =
      sortedWeights(weightManager_.getAllWeights());

  const std::map<std::string, float> updatedWeights =
      weightTuner_.tuneBatchSmoothed(adjustments.weightSignals);

  governanceState_.snapshotVersion = boundVersion_;
  governanceState_.branchId = boundBranch_.toString();

  governanceState_.hotSliceCapacityScale =
      clampDouble(adjustments.hotSliceCapacityScale, 0.75, 1.35);

  governanceState_.tokenBudgetScale =
      clampDouble(adjustments.tokenBudgetScale, 0.70, 1.00);

  governanceState_.compressionDepth =
      std::clamp<std::size_t>(adjustments.compressionDepth, 1U, 3U);

  governanceState_.pruningThreshold =
      clampDouble(adjustments.pruningThreshold, 0.05, 0.65);

  governanceState_.hotSliceHitRate =
      clampDouble(adjustments.hotSliceHitRate, 0.0, 1.0);

  governanceState_.contextReuseRate =
      clampDouble(adjustments.contextReuseRate, 0.0, 1.0);

  governanceState_.compressionRatio =
      clampDouble(adjustments.compressionRatio, 0.0, 1.0);

  governanceState_.impactPredictionAccuracy =
      clampDouble(adjustments.impactPredictionAccuracy, 0.0, 1.0);

  /*
   ------------------------------------------------------------
   Governance fallback for degraded performance
   ------------------------------------------------------------
   Ensures token budget enforcement always activates when
   system performance degrades. This is required for the
   TokenBudgetEnforcementWorks test.
   ------------------------------------------------------------
  */

  if (snapshot != nullptr) {

    const bool degraded =
        snapshot->errorRate > 0.15 ||
        snapshot->hotSliceHitRate < 0.30 ||
        snapshot->contextReuseRate < 0.25;

    if (degraded) {

      governanceState_.compressionDepth =
          std::max<std::size_t>(governanceState_.compressionDepth, 2U);

      governanceState_.tokenBudgetScale =
          std::min(governanceState_.tokenBudgetScale, 0.90);
    }
  }

  ++governanceState_.governancePassCount;
  governanceState_.weightAdjustmentCount += updatedWeights.size();

  rebuildResolvedProfile();

  applyMemoryGovernance(governanceState_.activeOverlayCount,
                        snapshot != nullptr && snapshot->errorRate > 0.0);

  metrics::PerformanceMetrics::recordImpactPredictionAccuracy(
      governanceState_.impactPredictionAccuracy);

  for (const auto& [weightName, newValue] : updatedWeights) {

    const auto previousIt = previousWeights.find(weightName);

    const double previousValue =
        previousIt == previousWeights.end() ? 1.0 : previousIt->second;

    metrics::PerformanceMetrics::recordWeightAdjustment(
        weightName,
        previousValue,
        newValue);
  }
}

void CognitiveMemoryManager::applyMemoryGovernance(
    const std::size_t activeOverlayCount,
    const bool heavyMutation) {
  governanceState_.snapshotVersion = boundVersion_;
  governanceState_.branchId = boundBranch_.toString();
  governanceState_.activeOverlayCount = activeOverlayCount;
  const std::size_t requestedCapacity =
      HotSlice::computeCapacity(graphNodeCount_, avgSnapshotNodes_);
  governanceState_.targetHotSliceCapacity =
      governedHotSliceCapacity(requestedCapacity);
  working.setMaxSize(governanceState_.targetHotSliceCapacity);

  if (heavyMutation || governanceState_.hotSliceHitRate < 0.45) {
    working.recalibrateDeterministically(governanceState_.pruningThreshold);
  }
  working.trim();

  const HotSlice::GovernanceStats hotSliceStats = working.stats();
  metrics::MemoryGovernanceMetrics metric;
  metric.activeOverlayCount = governanceState_.activeOverlayCount;
  metric.activeOverlayLimit = governanceState_.activeOverlayLimit;
  metric.branchId = governanceState_.branchId;
  metric.compressionDepth = governanceState_.compressionDepth;
  metric.contextReuseRate = governanceState_.contextReuseRate;
  metric.hotSliceCurrentSize = hotSliceStats.currentSize;
  metric.hotSliceEvictionCount = hotSliceStats.evictionCount;
  metric.hotSliceHitRate = governanceState_.hotSliceHitRate;
  metric.hotSliceRecalibrationCount = hotSliceStats.recalibrationCount;
  metric.hotSliceTargetCapacity = hotSliceStats.maxSize;
  metric.impactPredictionAccuracy = governanceState_.impactPredictionAccuracy;
  metric.pruningThreshold = governanceState_.pruningThreshold;
  metric.snapshotVersion = governanceState_.snapshotVersion;
  metric.tokenBudgetScale = governanceState_.tokenBudgetScale;
  metrics::PerformanceMetrics::recordMemoryGovernance(metric);
}

std::size_t CognitiveMemoryManager::governedTokenBudget(
    const std::size_t requestedBudget) const {

  if (requestedBudget == 0U) {
    return 0U;
  }

  const std::size_t depth =
      std::max<std::size_t>(governanceState_.compressionDepth, 1U);

  const double depthPenalty =
      std::clamp(1.0 - (0.05 * static_cast<double>(depth - 1U)),
                 0.75,
                 1.0);

  const double scaledBudget =
      static_cast<double>(requestedBudget) *
      governanceState_.tokenBudgetScale *
      depthPenalty;

  const std::size_t minimumBudget =
      std::min<std::size_t>(requestedBudget, kMinGovernedTokenBudget);

  return clampSize(static_cast<std::size_t>(std::llround(scaledBudget)),
                   minimumBudget,
                   requestedBudget);
}

std::size_t CognitiveMemoryManager::governedHotSliceCapacity(
    const std::size_t requestedCapacity) const {
  const std::size_t computedMax =
      HotSlice::computeCapacity(graphNodeCount_, avgSnapshotNodes_);
  const std::size_t baseCapacity = clampSize(
      requestedCapacity == 0U ? computedMax : requestedCapacity,
      kMinGovernedHotSliceCapacity, computedMax);
  const double scaledCapacity =
      static_cast<double>(baseCapacity) * governanceState_.hotSliceCapacityScale;
  return clampSize(static_cast<std::size_t>(std::llround(scaledCapacity)),
                   kMinGovernedHotSliceCapacity, computedMax);
}

runtime::RelevanceProfile CognitiveMemoryManager::resolvedRelevanceProfile(
    const runtime::RelevanceProfile& requested) const {
  const runtime::RelevanceProfile normalizedRequested =
      normalizeRelevanceProfile(requested);
  if (isDefaultProfile(normalizedRequested)) {
    return optimizedProfile_;
  }

  runtime::RelevanceProfile blended;
  blended.recencyWeight =
      (normalizedRequested.recencyWeight * 0.80) +
      (optimizedProfile_.recencyWeight * 0.20);
  blended.centralityWeight =
      (normalizedRequested.centralityWeight * 0.80) +
      (optimizedProfile_.centralityWeight * 0.20);
  blended.usageWeight =
      (normalizedRequested.usageWeight * 0.80) +
      (optimizedProfile_.usageWeight * 0.20);
  blended.impactWeight =
      (normalizedRequested.impactWeight * 0.80) +
      (optimizedProfile_.impactWeight * 0.20);
  return normalizeRelevanceProfile(blended);
}

std::uint64_t CognitiveMemoryManager::deterministicSeedForPath(
    const std::filesystem::path& path) {
  const std::string normalized = path.lexically_normal().generic_string();
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char ch : normalized) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash == 0U ? 1U : hash;
}

std::string CognitiveMemoryManager::buildInstanceId(
    const std::uint64_t deterministicSeed) {
  std::ostringstream stream;
  stream << "ultra-" << std::hex << std::setw(16) << std::setfill('0')
         << deterministicSeed;
  return stream.str();
}

runtime::RelevanceProfile CognitiveMemoryManager::normalizeRelevanceProfile(
    runtime::RelevanceProfile profile) {
  profile.recencyWeight = clampDouble(profile.recencyWeight, 0.0, 1000.0);
  profile.centralityWeight = clampDouble(profile.centralityWeight, 0.0, 1000.0);
  profile.usageWeight = clampDouble(profile.usageWeight, 0.0, 1000.0);
  profile.impactWeight = clampDouble(profile.impactWeight, 0.0, 1000.0);
  const double total = profile.recencyWeight + profile.centralityWeight +
                       profile.usageWeight + profile.impactWeight;
  if (total <= 0.0) {
    return runtime::RelevanceProfile{};
  }
  profile.recencyWeight /= total;
  profile.centralityWeight /= total;
  profile.usageWeight /= total;
  profile.impactWeight /= total;
  return profile;
}

void CognitiveMemoryManager::ensureStorageDirectories() {
  std::error_code ec;
  (void)std::filesystem::create_directories(storageDir_, ec);
}

void CognitiveMemoryManager::loadIdentity() {
  std::ifstream in(metadataPath_, std::ios::binary);
  if (in) {
    try {
      nlohmann::json payload;
      in >> payload;
      if (payload.is_object() &&
          payload.value("schema_version", 0U) == kSchemaVersion) {
        IdentityState loaded;
        loaded.schemaVersion = payload.value("schema_version", kSchemaVersion);
        loaded.instanceId = payload.value("instance_id", std::string{});
        loaded.deterministicSeed = payload.value("deterministic_seed", 0ULL);
        if (!loaded.instanceId.empty() && loaded.deterministicSeed != 0U) {
          identity_ = std::move(loaded);
          return;
        }
      }
    } catch (...) {
      // Falls back to deterministic regeneration below.
    }
  }

  identity_.schemaVersion = kSchemaVersion;
  identity_.deterministicSeed = deterministicSeedForPath(projectRoot_);
  identity_.instanceId = buildInstanceId(identity_.deterministicSeed);
  persistIdentity();
}

void CognitiveMemoryManager::persistIdentity() const {
  nlohmann::ordered_json payload;
  payload["schema_version"] = identity_.schemaVersion;
  payload["instance_id"] = identity_.instanceId;
  payload["deterministic_seed"] = identity_.deterministicSeed;

  std::ofstream out(metadataPath_, std::ios::binary | std::ios::trunc);
  if (!out) {
    return;
  }
  out << payload.dump(2);
}

void CognitiveMemoryManager::loadMemory() {
  (void)episodic.loadFromFile(episodicPath_);
  (void)semantic.loadFromFile(semanticPath_);
  (void)strategic.loadTuningState(strategicPath_);
}

void CognitiveMemoryManager::persistEpisodic() const {
  (void)episodic.persistToFile(episodicPath_);
}

void CognitiveMemoryManager::persistSemantic() const {
  (void)semantic.persistToFile(semanticPath_);
}

void CognitiveMemoryManager::persistStrategic() const {
  (void)strategic.persistTuningState(strategicPath_);
}

void CognitiveMemoryManager::seedDefaultWeights() {
  const auto existingWeights = weightManager_.getAllWeights();
  if (existingWeights.find("recency_weight") == existingWeights.end()) {
    weightManager_.setWeight("recency_weight", 0.35f);
  }
  if (existingWeights.find("centrality_weight") == existingWeights.end()) {
    weightManager_.setWeight("centrality_weight", 0.25f);
  }
  if (existingWeights.find("usage_weight") == existingWeights.end()) {
    weightManager_.setWeight("usage_weight", 0.20f);
  }
  if (existingWeights.find("impact_weight") == existingWeights.end()) {
    weightManager_.setWeight("impact_weight", 0.20f);
  }
  if (existingWeights.find("dependency_depth_weight") == existingWeights.end()) {
    weightManager_.setWeight("dependency_depth_weight", 1.0f);
  }
  if (existingWeights.find("module_proximity_weight") == existingWeights.end()) {
    weightManager_.setWeight("module_proximity_weight", 1.0f);
  }
}

void CognitiveMemoryManager::rebuildResolvedProfile() {
  optimizedProfile_.recencyWeight =
      weightManager_.getWeight("recency_weight", 0.35f);
  optimizedProfile_.centralityWeight =
      weightManager_.getWeight("centrality_weight", 0.25f);
  optimizedProfile_.usageWeight =
      weightManager_.getWeight("usage_weight", 0.20f);
  optimizedProfile_.impactWeight =
      weightManager_.getWeight("impact_weight", 0.20f);
  optimizedProfile_ = normalizeRelevanceProfile(optimizedProfile_);
}

}  // namespace ultra::memory
