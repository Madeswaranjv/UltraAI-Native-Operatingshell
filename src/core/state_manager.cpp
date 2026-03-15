#include "state_manager.h"

#include "../ai/Hashing.h"
#include "Logger.h"
#include "../memory/epoch/EpochManager.h"
#include "../metrics/PerformanceMetrics.h"
#include "../runtime/CPUGovernor.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ultra::core {

namespace {

constexpr std::size_t kHotWindow = 8U;
constexpr std::size_t kHotSliceFloor = 10000U;
constexpr std::size_t kMaxHotSymbols = 64U;
constexpr std::size_t kTokenBudgetCeiling = 1U << 20;

std::size_t estimateSnapshotPayloadBytes(const memory::StateSnapshot& snapshot) {
  std::size_t total =
      sizeof(snapshot.id) + sizeof(snapshot.nodeCount) +
      sizeof(snapshot.edgeCount) + snapshot.snapshotId.size() +
      snapshot.graphHash.size();

  for (const memory::StateNode& node : snapshot.nodes) {
    total += sizeof(node.nodeType) + sizeof(node.version) + node.nodeId.size() +
             node.data.dump().size();
  }
  for (const memory::StateEdge& edge : snapshot.edges) {
    total += sizeof(edge.edgeType) + sizeof(edge.weight) + edge.edgeId.size() +
             edge.sourceId.size() + edge.targetId.size();
  }

  return total;
}

std::string fileNodeId(const std::string& path) {
  return "file:" + path;
}

std::string symbolNodeId(const std::uint64_t symbolId) {
  std::ostringstream stream;
  stream << "symbol:" << symbolId;
  return stream.str();
}

std::vector<std::string> sortedUsedIn(
    const std::unordered_set<std::string>& usedInFiles) {
  std::vector<std::string> out(usedInFiles.begin(), usedInFiles.end());
  std::sort(out.begin(), out.end());
  return out;
}

std::uint64_t deterministicFallbackSymbolId(const std::string& symbolName) {
  const ai::Sha256Hash digest = ai::sha256OfString(symbolName);
  std::uint64_t id = 0U;
  for (std::size_t index = 0U; index < sizeof(id); ++index) {
    id = (id << 8U) | static_cast<std::uint64_t>(digest[index]);
  }
  return id;
}

template <typename T>
void deleteSharedPtrHolder(void* holder) {
  delete static_cast<std::shared_ptr<T>*>(holder);
}

template <typename T>
void retireSharedPtr(std::shared_ptr<T>&& pointer) {
  if (!pointer) {
    return;
  }
  auto* holder = new std::shared_ptr<T>(std::move(pointer));
  memory::epoch::EpochManager::instance().retire(
      static_cast<void*>(holder), &deleteSharedPtrHolder<T>);
}

template <typename T>
void replaceSharedPtr(std::shared_ptr<T>& slot, std::shared_ptr<T> replacement) {
  if (slot.get() == replacement.get()) {
    slot = std::move(replacement);
    return;
  }
  std::shared_ptr<T> previous = std::move(slot);
  slot = std::move(replacement);
  retireSharedPtr(std::move(previous));
}

struct HotSliceSnapshot {
  std::size_t maxSize{memory::HotSlice::kMaxHotSliceEntries};
  std::uint64_t boundVersion{0U};
  std::vector<memory::StateNode> nodes;
};

HotSliceSnapshot snapshotHotSlice(const memory::HotSlice& hotSlice) {
  HotSliceSnapshot snapshot;
  snapshot.maxSize = hotSlice.maxSize();
  snapshot.boundVersion = hotSlice.boundSnapshotVersion();
  const std::size_t count = hotSlice.currentSize();
  if (count != 0U) {
    snapshot.nodes = hotSlice.getTopK(count);
  }
  return snapshot;
}

void restoreHotSlice(memory::HotSlice& hotSlice, const HotSliceSnapshot& snapshot) {
  hotSlice.~HotSlice();
  new (&hotSlice) memory::HotSlice(snapshot.maxSize);
  if (snapshot.boundVersion != 0U) {
    hotSlice.bindToSnapshotVersion(snapshot.boundVersion);
  }
  for (const memory::StateNode& node : snapshot.nodes) {
    hotSlice.storeNode(node, snapshot.boundVersion);
  }
}

void resetHotSlice(memory::HotSlice& hotSlice, const std::size_t maxSize) {
  hotSlice.~HotSlice();
  new (&hotSlice) memory::HotSlice(maxSize);
}

std::shared_ptr<memory::HotSlice> aliasHotSlice(memory::HotSlice& hotSlice) {
  return std::shared_ptr<memory::HotSlice>(&hotSlice, [](memory::HotSlice*) {});
}

}  // namespace

StateManager::StateManager(const std::filesystem::path& projectRoot)
    : graphStore_(projectRoot / ".ultra" /"graph"),
      symbolQueryEngine_(&graphStore_),
      cognitiveMemoryManager_(projectRoot) {}

std::size_t StateManager::clampTokenBudget(const std::size_t tokenBudget) {
  if (tokenBudget == 0U) {
    return 0U;
  }
  return std::min(tokenBudget, kTokenBudgetCeiling);
}

void StateManager::runMemoryGovernanceLocked(const bool heavyMutation) {
  snapshotChain_.enforceRetentionCap();
  if (!activeBranch_.isNil()) {
    branchOverlayLruManager_.touch(activeBranch_.toString());
  }
  cognitiveMemoryManager_.applyMemoryGovernance(branchOverlayLruManager_.size(),
                                                heavyMutation);
  if (heavyMutation) {
    runtime::CPUGovernor::instance().enterIdle();
    runtime::CPUGovernor::instance().exitIdle();
  }
}

void StateManager::replaceState(ai::RuntimeState state) {
  std::unique_lock lock(graphMutex_);
  state_ = std::move(state);
  weightEngine_.rebuildFromState(state_);
  lruManager_.rebuild(state_.files);
  ++globalVersion_;
  rebuildGraphLocked();
  symbolQueryEngine_.rebuild(state_, globalVersion_);
  const bool metricsEnabled = metrics::PerformanceMetrics::isEnabled();
  const auto snapshotStartedAt =
      metricsEnabled ? std::chrono::steady_clock::now()
                     : std::chrono::steady_clock::time_point{};
  const memory::StateSnapshot snapshot = graph_->snapshot(globalVersion_);
  snapshotChain_.append(snapshot);
  if (metricsEnabled) {
    metrics::SnapshotMetrics metric;
    metric.operation = "replace_state_snapshot_generation";
    metric.durationMicros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - snapshotStartedAt)
            .count());
    metric.nodeCount = snapshot.nodeCount;
    metric.edgeCount = snapshot.edgeCount;
    metric.snapshotSizeBytes = estimateSnapshotPayloadBytes(snapshot);
    metrics::PerformanceMetrics::recordSnapshotMetric(metric);
  }
  runMemoryGovernanceLocked(false);
}

void StateManager::updateCore(const ai::CoreIndex& core) {
  std::unique_lock lock(graphMutex_);
  state_.core = core;
  ++globalVersion_;
  rebuildGraphLocked();
  symbolQueryEngine_.rebuild(state_, globalVersion_);
  snapshotChain_.append(graph_->snapshot(globalVersion_));
  runMemoryGovernanceLocked(false);
}

void StateManager::setActiveBranch(const runtime::BranchId& branch) {
  std::unique_lock lock(graphMutex_);
  activeBranch_ = branch;
  branchOverlayLruManager_.touch(branch.toString());
  ++globalVersion_;
  rebuildGraphLocked();
  symbolQueryEngine_.rebuild(state_, globalVersion_);
  snapshotChain_.append(graph_->snapshot(globalVersion_));
  runMemoryGovernanceLocked(true);
}

ai::RuntimeState StateManager::snapshotState() const {
  std::shared_lock lock(graphMutex_);
  return state_;
}

RuntimeStatusSnapshot StateManager::snapshotStatus(
    const std::size_t pendingChanges) const {
  std::shared_lock lock(graphMutex_);
  RuntimeStatusSnapshot snapshot;
  snapshot.runtimeActive = state_.core.runtimeActive == 1U;
  snapshot.filesIndexed = state_.files.size();
  snapshot.symbolsIndexed = state_.symbols.size();
  snapshot.dependenciesIndexed =
      state_.deps.fileEdges.size() + state_.deps.symbolEdges.size();
  snapshot.pendingChanges = pendingChanges;
  snapshot.weightsTracked = weightEngine_.trackedCount();
  snapshot.lruTracked = lruManager_.size();
  snapshot.schemaVersion = state_.core.schemaVersion;
  snapshot.indexVersion = state_.core.indexVersion;
  return snapshot;
}

runtime::GraphSnapshot StateManager::getSnapshot() const {
  std::shared_lock lock(graphMutex_);
  runtime::GraphSnapshot snapshot;
  snapshot.graph = graph_;
  snapshot.runtimeState = std::make_shared<ai::RuntimeState>(state_);
  snapshot.graphStore = const_cast<graph_store::GraphStore*>(&graphStore_);
  snapshot.version = globalVersion_;
  snapshot.branch = activeBranch_;
  return snapshot;
}

runtime::CognitiveState StateManager::createCognitiveState(
    const std::size_t tokenBudget,
    const runtime::RelevanceProfile& weights) const {
  const std::size_t boundedTokenBudget =
      cognitiveMemoryManager_.governedTokenBudget(clampTokenBudget(tokenBudget));
  runtime::GraphSnapshot snapshot = getSnapshot();
  std::shared_ptr<memory::HotSlice> workingSet;

  {
    std::shared_lock lock(graphMutex_);
    if (snapshot.version != globalVersion_) {
      snapshot.graph = graph_;
      snapshot.version = globalVersion_;
      snapshot.branch = activeBranch_;
    }
    workingSet = aliasHotSlice(cognitiveMemoryManager_.working);
  }

  cognitiveMemoryManager_.bindToSnapshot(&snapshot);
  workingSet->bindToSnapshotVersion(snapshot.version);
  workingSet->syncVersions(snapshot.version);
  const runtime::RelevanceProfile effectiveWeights =
      cognitiveMemoryManager_.resolvedRelevanceProfile(weights);
  return runtime::CognitiveState(snapshot, std::move(workingSet), boundedTokenBudget,
                                 effectiveWeights);
}

std::vector<engine::query::SymbolDefinition> StateManager::findDefinition(
    const std::string& symbolName) const {
  std::shared_lock lock(graphMutex_);
  return symbolQueryEngine_.findDefinition(symbolName);
}

std::vector<std::string> StateManager::findReferences(
    const std::string& symbolName) const {
  std::shared_lock lock(graphMutex_);
  return symbolQueryEngine_.findReferences(symbolName);
}

std::vector<std::string> StateManager::findFileDependencies(
    const std::string& filePath) const {
  std::shared_lock lock(graphMutex_);
  return symbolQueryEngine_.findFileDependencies(filePath);
}

std::vector<std::string> StateManager::findSymbolDependencies(
    const std::string& symbolName) const {
  std::shared_lock lock(graphMutex_);
  return symbolQueryEngine_.findSymbolDependencies(symbolName);
}

std::vector<std::string> StateManager::findImpactRegion(
    const std::string& symbolName,
    const std::size_t maxDepth) const {
  std::shared_lock lock(graphMutex_);
  return symbolQueryEngine_.findImpactRegion(symbolName, maxDepth);
}

std::uint64_t StateManager::currentVersion() const {
  std::shared_lock lock(graphMutex_);
  return globalVersion_;
}

MemoryStats StateManager::getMemoryStats() const {
  std::shared_lock lock(graphMutex_);
  MemoryStats stats;
  stats.hotSliceSize = cognitiveMemoryManager_.working.currentSize();
  stats.snapshotCount = snapshotChain_.size();
  stats.activeBranchCount = branchOverlayLruManager_.size();
  return stats;
}

bool StateManager::loadPersistedGraph(std::string& error,
                                      const std::size_t maxChunks) {
  std::unique_lock lock(graphMutex_);

  ai::RuntimeState loadedState;
  const bool loaded = maxChunks == 0U
                          ? graphStore_.load(loadedState, error)
                          : graphStore_.loadPartial(maxChunks, loadedState, error);
  if (!loaded) {
    return false;
  }

  state_ = std::move(loadedState);
  weightEngine_.rebuildFromState(state_);
  lruManager_.rebuild(state_.files);
  ++globalVersion_;
  rebuildGraphLocked();
  symbolQueryEngine_.rebuild(state_, globalVersion_);
  snapshotChain_.clear();
  snapshotChain_.append(graph_->snapshot(globalVersion_));
  runMemoryGovernanceLocked(false);
  return true;
}

bool StateManager::persistGraphStore(std::string& error) {
  std::shared_lock lock(graphMutex_);
  return graphStore_.persistFull(state_, error);
}

bool StateManager::persistGraphStoreIncremental(
    const std::vector<std::uint32_t>& touchedFileIds,
    std::string& error) {
  std::shared_lock lock(graphMutex_);
  return graphStore_.applyIncremental(state_, touchedFileIds, error);
}

memory::CognitiveMemoryManager& StateManager::cognitiveMemory() noexcept {
  return cognitiveMemoryManager_;
}

const memory::CognitiveMemoryManager& StateManager::cognitiveMemory() const
    noexcept {
  return cognitiveMemoryManager_;
}

KernelHealthSnapshot StateManager::verifyKernelHealth() const {
  std::shared_lock lock(graphMutex_);

  KernelHealthSnapshot health;
  health.branchCount = branchOverlayLruManager_.size();
  health.snapshotCount = snapshotChain_.size();
  health.governanceActive = true;
  health.determinismGuardsActive = false;

  if (!graph_) {
    health.violations.push_back("Overlay graph pointer is null.");
  } else {
    const std::string deterministicHash = graph_->getDeterministicHash();
    if (deterministicHash.empty()) {
      health.violations.push_back("Deterministic graph hash is empty.");
    } else {
      health.determinismGuardsActive = true;
    }
  }

  if (activeBranch_.toString().empty()) {
    health.violations.push_back("Active branch identifier is empty.");
  }

  if (health.snapshotCount == 0U) {
    health.violations.push_back("Snapshot chain is empty.");
  }
  if (health.snapshotCount > memory::SnapshotChain::kMaxSnapshotsRetained) {
    health.violations.push_back("Snapshot retention cap exceeded.");
  }
  if (health.branchCount > memory::LruManager::kMaxActiveBranches) {
    health.violations.push_back("Active branch cap exceeded.");
  }
  if (clampTokenBudget(kTokenBudgetCeiling + 1U) != kTokenBudgetCeiling) {
    health.violations.push_back("Token budget ceiling enforcement is invalid.");
  }

  health.memoryCapsRespected =
      health.snapshotCount <= memory::SnapshotChain::kMaxSnapshotsRetained &&
      health.branchCount <= memory::LruManager::kMaxActiveBranches;
  health.healthy = health.violations.empty();

  if (!health.healthy) {
    for (const std::string& violation : health.violations) {
      Logger::error(LogCategory::General,
                    "Kernel invariant violation: " + violation);
    }
  }

  return health;
}

void StateManager::ensureSnapshotCurrent(
    const runtime::GraphSnapshot& snapshot) const {
  std::shared_lock lock(graphMutex_);
  if (snapshot.version != globalVersion_) {
    throw std::runtime_error("Snapshot version mismatch. Request a fresh snapshot.");
  }
}

KernelMutationOutcome StateManager::applyOverlayMutation(
    const runtime::GraphSnapshot& expectedSnapshot,
    const WriteMutation& mutation) {
  if (!mutation) {
    return KernelMutationOutcome{
        false, false, 0U, 0U, {}, {}, "Overlay mutation callback is empty."};
  }

  std::unique_lock lock(graphMutex_);
  if (expectedSnapshot.version != globalVersion_) {
    return KernelMutationOutcome{false,
                                 false,
                                 globalVersion_,
                                 globalVersion_,
                                 graph_ ? graph_->getDeterministicHash()
                                        : std::string{},
                                 graph_ ? graph_->getDeterministicHash()
                                        : std::string{},
                                 "Snapshot version mismatch. Request a fresh snapshot."};
  }

  KernelMutationOutcome outcome;
  outcome.versionBefore = globalVersion_;
  outcome.versionAfter = globalVersion_;
  outcome.hashBefore = graph_ ? graph_->getDeterministicHash() : std::string{};
  if (!outcome.hashBefore.empty()) {
    std::string expectedHash;
    try {
      expectedHash = expectedSnapshot.deterministicHash();
    } catch (...) {
      return KernelMutationOutcome{
          false,
          false,
          globalVersion_,
          globalVersion_,
          outcome.hashBefore,
          outcome.hashBefore,
          "Snapshot hash mismatch. Request a fresh snapshot."};
    }
    if (expectedHash != outcome.hashBefore) {
      return KernelMutationOutcome{
          false,
          false,
          globalVersion_,
          globalVersion_,
          outcome.hashBefore,
          outcome.hashBefore,
          "Snapshot hash mismatch. Request a fresh snapshot."};
    }
  }

  const ai::RuntimeState previousState = state_;
  const engine::WeightEngine previousWeightEngine = weightEngine_;
  const memory::LruManager previousLru = lruManager_;
  const memory::LruManager previousBranchOverlayLru = branchOverlayLruManager_;
  const HotSliceSnapshot previousHotSlice =
      snapshotHotSlice(cognitiveMemoryManager_.working);
  const memory::SnapshotChain previousSnapshotChain = snapshotChain_;
  const std::shared_ptr<memory::StateGraph> previousGraph =
      graph_ ? std::make_shared<memory::StateGraph>(*graph_) : nullptr;
  const runtime::DiffResult previousDiffResult = pendingDiffResult_;
  const std::uint64_t previousVersion = globalVersion_;
  const runtime::BranchId previousBranch = activeBranch_;

  const auto rollback = [&]() {
    state_ = previousState;
    weightEngine_ = previousWeightEngine;
    lruManager_ = previousLru;
    branchOverlayLruManager_ = previousBranchOverlayLru;
    restoreHotSlice(cognitiveMemoryManager_.working, previousHotSlice);
    snapshotChain_ = previousSnapshotChain;
    replaceSharedPtr(graph_, previousGraph);
    pendingDiffResult_ = previousDiffResult;
    activeBranch_ = previousBranch;
    globalVersion_ = previousVersion;
    outcome.applied = false;
    outcome.rolledBack = true;
    outcome.versionAfter = globalVersion_;
    outcome.hashAfter = graph_ ? graph_->getDeterministicHash() : std::string{};
  };

  const bool mutationApplied = mutation(state_, weightEngine_, lruManager_);
  if (!mutationApplied) {
    rollback();
    outcome.message = "Overlay mutation was rejected.";
    return outcome;
  }

  const runtime::DiffResult diffResult = runtime::buildDiffResult(
      previousState, state_, &cognitiveMemoryManager_.semantic, globalVersion_ + 1U);
  const runtime::StructuralChangeType changeType = runtime::classifyChange(diffResult);
  pendingDiffResult_ = diffResult;
  ++globalVersion_;
  applyPrecisionInvalidation(changeType, diffResult.affectedSymbols);

  const bool metricsEnabled = metrics::PerformanceMetrics::isEnabled();
  const auto snapshotStartedAt =
      metricsEnabled ? std::chrono::steady_clock::now()
                     : std::chrono::steady_clock::time_point{};
  const memory::StateSnapshot snapshot = graph_->snapshot(globalVersion_);
  snapshotChain_.append(snapshot);
  if (metricsEnabled) {
    metrics::SnapshotMetrics metric;
    metric.operation = "overlay_mutation_snapshot_generation";
    metric.durationMicros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - snapshotStartedAt)
            .count());
    metric.nodeCount = snapshot.nodeCount;
    metric.edgeCount = snapshot.edgeCount;
    metric.snapshotSizeBytes = estimateSnapshotPayloadBytes(snapshot);
    metrics::PerformanceMetrics::recordSnapshotMetric(metric);
  }
  runMemoryGovernanceLocked(true);

  const std::string hashAfter = graph_ ? graph_->getDeterministicHash() : std::string{};
  const std::string hashAfterRecheck =
      graph_ ? graph_->getDeterministicHash() : std::string{};
  if (hashAfter.empty() || hashAfter != hashAfterRecheck) {
    rollback();
    runtime::GraphSnapshot failedSnapshot;
    failedSnapshot.graph = graph_;
    failedSnapshot.version = globalVersion_;
    failedSnapshot.branch = activeBranch_;
    cognitiveMemoryManager_.recordRiskEvaluation(
        "diff_risk:overlay_mutation", failedSnapshot,
        diffResult.delta.riskScore.overallRisk, 1.00, 0.10,
        "hash_integrity_failure");
    outcome.message = "Overlay hash integrity verification failed; mutation rolled back.";
    return outcome;
  }

  outcome.applied = true;
  outcome.rolledBack = false;
  outcome.versionAfter = globalVersion_;
  outcome.hashAfter = hashAfter;
  outcome.message = "Overlay mutation committed.";
  symbolQueryEngine_.rebuild(state_, globalVersion_);

  runtime::GraphSnapshot committedSnapshot;
  committedSnapshot.graph = graph_;
  committedSnapshot.version = globalVersion_;
  committedSnapshot.branch = activeBranch_;
  cognitiveMemoryManager_.bindToSnapshot(&committedSnapshot);
  cognitiveMemoryManager_.recordRiskEvaluation(
      "diff_risk:overlay_mutation", committedSnapshot,
      diffResult.delta.riskScore.overallRisk, 0.20, 0.80,
      "overlay_mutation_committed");

  std::map<std::uint64_t, ai::SymbolRecord> symbolById;
  for (const ai::SymbolRecord& symbol : state_.symbols) {
    if (symbol.symbolId == 0ULL) {
      continue;
    }
    const auto it = symbolById.find(symbol.symbolId);
    if (it == symbolById.end() || symbol.name < it->second.name) {
      symbolById[symbol.symbolId] = symbol;
    }
  }

  for (const auto& [symbolId, renamedTo] : pendingDiffResult_.renamedSymbols) {
    if (symbolId == 0ULL) {
      continue;
    }
    std::string signature;
    const auto symbolIt = symbolById.find(symbolId);
    if (symbolIt != symbolById.end()) {
      signature = symbolIt->second.signature;
    }
    cognitiveMemoryManager_.updateSemanticEvolution(
        symbolNodeId(symbolId), renamedTo, signature, "rename", globalVersion_);
  }

  if (changeType == runtime::StructuralChangeType::SIGNATURE_CHANGE) {
    for (const runtime::SymbolID symbolId : diffResult.affectedSymbols) {
      if (symbolId == 0ULL) {
        continue;
      }
      const auto symbolIt = symbolById.find(symbolId);
      if (symbolIt == symbolById.end()) {
        continue;
      }
      cognitiveMemoryManager_.updateSemanticEvolution(
          symbolNodeId(symbolId), symbolIt->second.name, symbolIt->second.signature,
          "signature_change", globalVersion_);
    }
  }

  if (changeType == runtime::StructuralChangeType::API_REMOVAL) {
    for (const runtime::SymbolID symbolId : diffResult.affectedSymbols) {
      if (symbolId == 0ULL) {
        continue;
      }
      const auto symbolIt = symbolById.find(symbolId);
      const std::string removedName =
          symbolIt == symbolById.end() ? std::string{} : symbolIt->second.name;
      cognitiveMemoryManager_.updateSemanticEvolution(
          symbolNodeId(symbolId), removedName, std::string{}, "removed",
          globalVersion_);
    }
  }

  return outcome;
}

void StateManager::withReadLock(const ReadView& view) const {
  std::shared_lock lock(graphMutex_);
  view(state_, weightEngine_, lruManager_);
}

bool StateManager::withWriteLock(const WriteMutation& mutation) {
  const runtime::GraphSnapshot expectedSnapshot = getSnapshot();
  const KernelMutationOutcome outcome =
      applyOverlayMutation(expectedSnapshot, mutation);
  return outcome.applied;
}

void StateManager::applyPrecisionInvalidation(
    const runtime::StructuralChangeType changeType,
    const std::vector<runtime::SymbolID>& affectedSymbols) {
  std::vector<runtime::SymbolID> sortedAffected = affectedSymbols;
  std::sort(sortedAffected.begin(), sortedAffected.end());
  sortedAffected.erase(std::unique(sortedAffected.begin(), sortedAffected.end()),
                       sortedAffected.end());

  cognitiveMemoryManager_.working.syncVersions(globalVersion_);

  const auto invalidateSymbols =
      [this](const std::set<runtime::SymbolID>& ids) {
        for (const runtime::SymbolID symbolId : ids) {
          cognitiveMemoryManager_.working.markStale(symbolNodeId(symbolId),
                                                    globalVersion_);
        }
      };

  if (!graph_) {
    rebuildGraphLocked();
    return;
  }

  const std::uint32_t snapshotNodeVersion =
      static_cast<std::uint32_t>(globalVersion_ & 0xFFFFFFFFULL);

  std::map<std::string, std::uint64_t> symbolIdByName;
  for (const ai::SymbolRecord& symbol : state_.symbols) {
    const auto symbolIt = symbolIdByName.find(symbol.name);
    if (symbolIt == symbolIdByName.end() || symbol.symbolId < symbolIt->second) {
      symbolIdByName[symbol.name] = symbol.symbolId;
    }
  }

  std::map<std::uint64_t, std::pair<std::string, ai::SymbolNode>> symbolById;
  std::vector<std::string> symbolNames;
  symbolNames.reserve(state_.symbolIndex.size());
  for (const auto& [name, symbolNode] : state_.symbolIndex) {
    (void)symbolNode;
    symbolNames.push_back(name);
  }
  std::sort(symbolNames.begin(), symbolNames.end());
  for (const std::string& name : symbolNames) {
    const ai::SymbolNode& symbolNode = state_.symbolIndex.at(name);
    const auto idIt = symbolIdByName.find(name);
    const std::uint64_t symbolId =
        idIt == symbolIdByName.end() ? deterministicFallbackSymbolId(name)
                                     : idIt->second;
    symbolById[symbolId] = {name, symbolNode};
  }

  std::set<std::string> knownPaths;
  for (const ai::FileRecord& file : state_.files) {
    knownPaths.insert(file.path);
  }

  std::map<std::string, ai::FileRecord> fileByPath;
  for (const ai::FileRecord& file : state_.files) {
    fileByPath[file.path] = file;
  }

  const std::vector<std::string> lruOrder = lruManager_.snapshot();
  std::map<std::string, std::size_t> hotRankByPath;
  for (std::size_t index = 0U; index < lruOrder.size(); ++index) {
    hotRankByPath.emplace(lruOrder[index], index);
  }

  const auto refreshChangedFiles = [&](memory::StateGraph& targetGraph) {
    for (const std::string& changedPath : pendingDiffResult_.changedFiles) {
      const auto fileIt = fileByPath.find(changedPath);
      if (fileIt == fileByPath.end()) {
        targetGraph.removeNode(fileNodeId(changedPath));
        continue;
      }

      const ai::FileRecord& file = fileIt->second;
      const std::string nodeId = fileNodeId(file.path);
      memory::StateNode node = targetGraph.getNode(nodeId);
      if (node.nodeId.empty()) {
        node.nodeId = nodeId;
        node.nodeType = memory::NodeType::File;
      }
      node.version = snapshotNodeVersion;

      const auto hotRankIt = hotRankByPath.find(file.path);
      const bool isKnownHot =
          hotRankIt != hotRankByPath.end() && hotRankIt->second < kHotWindow;

      node.data["kind"] = "file";
      node.data["path"] = file.path;
      node.data["file_id"] = file.fileId;
      node.data["hash"] = ai::hashToHex(file.hash);
      node.data["weight"] = weightEngine_.weightForPath(file.path);
      node.data["hot_rank"] =
          hotRankIt == hotRankByPath.end()
              ? static_cast<long long>(-1)
              : static_cast<long long>(hotRankIt->second);
      node.data["is_hot"] = isKnownHot;
      targetGraph.addNode(node);
    }
  };

  const auto removeUseEdgesToSymbol = [](memory::StateGraph& targetGraph,
                                         const std::string& symbolNodeIdValue) {
    for (const memory::StateNode& fileNode :
         targetGraph.queryByType(memory::NodeType::File)) {
      const std::vector<memory::StateEdge> outbound =
          targetGraph.getOutboundEdges(fileNode.nodeId);
      for (const memory::StateEdge& edge : outbound) {
        if (edge.targetId != symbolNodeIdValue) {
          continue;
        }
        if (edge.edgeId.rfind("use:", 0U) != 0U) {
          continue;
        }
        targetGraph.removeEdge(edge.edgeId);
      }
    }
  };

  switch (changeType) {
    case runtime::StructuralChangeType::BODY_CHANGE: {
      std::set<runtime::SymbolID> staleIds(sortedAffected.begin(),
                                           sortedAffected.end());
      invalidateSymbols(staleIds);
      if (!pendingDiffResult_.changedFiles.empty()) {
        auto workingGraph = std::make_shared<memory::StateGraph>(*graph_);
        refreshChangedFiles(*workingGraph);
        replaceSharedPtr(graph_, std::move(workingGraph));
      }
      return;
    }

    case runtime::StructuralChangeType::API_REMOVAL: {
      std::set<runtime::SymbolID> staleIds(sortedAffected.begin(),
                                           sortedAffected.end());
      invalidateSymbols(staleIds);
      for (const runtime::SymbolID symbolId : staleIds) {
        graph_->removeNode(symbolNodeId(symbolId));
      }
      rebuildGraphLocked();
      return;
    }

    case runtime::StructuralChangeType::SYMBOL_RENAME: {
      auto workingGraph = std::make_shared<memory::StateGraph>(*graph_);
      for (const auto& [symbolId, renamedTo] : pendingDiffResult_.renamedSymbols) {
        if (symbolId == 0ULL || renamedTo.empty()) {
          continue;
        }
        const std::string nodeId = symbolNodeId(symbolId);
        memory::StateNode node = workingGraph->getNode(nodeId);
        if (node.nodeId.empty()) {
          continue;
        }
        node.version = snapshotNodeVersion;
        node.data["name"] = renamedTo;
        workingGraph->addNode(node);
        cognitiveMemoryManager_.working.storeNode(node, globalVersion_);
      }
      refreshChangedFiles(*workingGraph);
      replaceSharedPtr(graph_, std::move(workingGraph));
      return;
    }

    case runtime::StructuralChangeType::SIGNATURE_CHANGE: {
      auto workingGraph = std::make_shared<memory::StateGraph>(*graph_);
      std::set<runtime::SymbolID> staleIds(sortedAffected.begin(),
                                           sortedAffected.end());

      for (const runtime::SymbolID symbolId : sortedAffected) {
        const auto symbolIt = symbolById.find(symbolId);
        const std::string nodeId = symbolNodeId(symbolId);
        if (symbolIt == symbolById.end()) {
          workingGraph->removeNode(nodeId);
          continue;
        }

        const std::string& symbolName = symbolIt->second.first;
        const ai::SymbolNode& symbolNode = symbolIt->second.second;
        const std::string definedIn = symbolNode.definedIn;
        const std::vector<std::string> usedIn =
            sortedUsedIn(symbolNode.usedInFiles);
        const double weight = definedIn.empty() ? symbolNode.weight
                                                : weightEngine_.weightForPath(definedIn);

        memory::StateNode graphNode = workingGraph->getNode(nodeId);
        if (graphNode.nodeId.empty()) {
          graphNode.nodeId = nodeId;
          graphNode.nodeType = memory::NodeType::Symbol;
        }
        graphNode.version = snapshotNodeVersion;
        graphNode.data["kind"] = "symbol";
        graphNode.data["symbol_id"] = symbolId;
        graphNode.data["name"] = symbolName;
        graphNode.data["defined_in"] = definedIn;
        graphNode.data["used_in"] = usedIn;
        graphNode.data["usage_count"] = usedIn.size();
        graphNode.data["weight"] = weight;
        graphNode.data["centrality"] = symbolNode.centrality;
        graphNode.data["is_hot"] = false;
        graphNode.data["hot_version"] = 0ULL;
        workingGraph->addNode(graphNode);
        cognitiveMemoryManager_.working.storeNode(graphNode, globalVersion_);

        const std::vector<memory::StateEdge> outbound =
            workingGraph->getOutboundEdges(nodeId);
        for (const memory::StateEdge& edge : outbound) {
          workingGraph->removeEdge(edge.edgeId);
        }
        if (!definedIn.empty() && knownPaths.find(definedIn) != knownPaths.end()) {
          memory::StateEdge defEdge;
          defEdge.edgeId = "def:" + nodeId + "->" + fileNodeId(definedIn);
          defEdge.sourceId = nodeId;
          defEdge.targetId = fileNodeId(definedIn);
          defEdge.edgeType = memory::EdgeType::Contains;
          workingGraph->addEdge(defEdge);
        }

        removeUseEdgesToSymbol(*workingGraph, nodeId);
        for (const std::string& usedInPath : usedIn) {
          if (knownPaths.find(usedInPath) == knownPaths.end()) {
            continue;
          }
          memory::StateEdge useEdge;
          useEdge.edgeId = "use:" + fileNodeId(usedInPath) + "->" + nodeId;
          useEdge.sourceId = fileNodeId(usedInPath);
          useEdge.targetId = nodeId;
          useEdge.edgeType = memory::EdgeType::DependsOn;
          workingGraph->addEdge(useEdge);
        }

        runtime::GraphSnapshot snapshot;
        snapshot.graph = workingGraph;
        snapshot.version = globalVersion_;
        snapshot.branch = activeBranch_;
        const std::vector<runtime::SymbolID> impacted =
            runtime::computeImpactDepthLimited(snapshot, symbolId, 2U);
        staleIds.insert(impacted.begin(), impacted.end());
      }

      invalidateSymbols(staleIds);
      refreshChangedFiles(*workingGraph);
      replaceSharedPtr(graph_, std::move(workingGraph));
      return;
    }

    case runtime::StructuralChangeType::DEPENDENCY_CHANGE: {
      auto workingGraph = std::make_shared<memory::StateGraph>(*graph_);
      std::set<std::string> impactedFiles;
      for (const auto& [fromPath, toPath] : pendingDiffResult_.removedDependencyEdges) {
        impactedFiles.insert(fromPath);
        impactedFiles.insert(toPath);
        workingGraph->removeEdge("dep:" + fromPath + "->" + toPath);
      }
      for (const auto& [fromPath, toPath] : pendingDiffResult_.addedDependencyEdges) {
        impactedFiles.insert(fromPath);
        impactedFiles.insert(toPath);
        memory::StateEdge edge;
        edge.edgeId = "dep:" + fromPath + "->" + toPath;
        edge.sourceId = fileNodeId(fromPath);
        edge.targetId = fileNodeId(toPath);
        edge.edgeType = memory::EdgeType::DependsOn;
        workingGraph->addEdge(edge);
      }

      std::set<std::string> centralityRegion = impactedFiles;
      std::map<std::uint32_t, std::string> pathByFileId;
      for (const ai::FileRecord& file : state_.files) {
        pathByFileId[file.fileId] = file.path;
      }
      for (const ai::FileDependencyEdge& edge : state_.deps.fileEdges) {
        const auto fromIt = pathByFileId.find(edge.fromFileId);
        const auto toIt = pathByFileId.find(edge.toFileId);
        if (fromIt == pathByFileId.end() || toIt == pathByFileId.end()) {
          continue;
        }
        if (impactedFiles.find(fromIt->second) == impactedFiles.end() &&
            impactedFiles.find(toIt->second) == impactedFiles.end()) {
          continue;
        }
        centralityRegion.insert(fromIt->second);
        centralityRegion.insert(toIt->second);
      }

      std::set<runtime::SymbolID> staleIds;
      for (const auto& [symbolId, symbolPair] : symbolById) {
        const ai::SymbolNode& symbolNode = symbolPair.second;
        bool inRegion =
            !symbolNode.definedIn.empty() &&
            centralityRegion.find(symbolNode.definedIn) != centralityRegion.end();
        if (!inRegion) {
          for (const std::string& path : symbolNode.usedInFiles) {
            if (centralityRegion.find(path) != centralityRegion.end()) {
              inRegion = true;
              break;
            }
          }
        }
        if (!inRegion) {
          continue;
        }

        const std::string nodeId = symbolNodeId(symbolId);
        memory::StateNode graphNode = workingGraph->getNode(nodeId);
        if (graphNode.nodeId.empty()) {
          continue;
        }
        graphNode.version = snapshotNodeVersion;
        graphNode.data["centrality"] = symbolNode.centrality;
        graphNode.data["weight"] = symbolNode.definedIn.empty()
                                       ? symbolNode.weight
                                       : weightEngine_.weightForPath(symbolNode.definedIn);
        graphNode.data["usage_count"] = symbolNode.usedInFiles.size();
        graphNode.data["used_in"] = sortedUsedIn(symbolNode.usedInFiles);
        workingGraph->addNode(graphNode);
        cognitiveMemoryManager_.working.storeNode(graphNode, globalVersion_);
        staleIds.insert(symbolId);
      }

      invalidateSymbols(staleIds);
      refreshChangedFiles(*workingGraph);
      replaceSharedPtr(graph_, std::move(workingGraph));
      return;
    }
  }
}

void StateManager::rebuildGraphLocked() {
  auto newGraph = std::make_shared<memory::StateGraph>();
  const std::uint32_t snapshotNodeVersion =
      static_cast<std::uint32_t>(globalVersion_ & 0xFFFFFFFFULL);

  std::vector<ai::FileRecord> files = state_.files;
  std::sort(files.begin(), files.end(),
            [](const ai::FileRecord& left, const ai::FileRecord& right) {
              if (left.path != right.path) {
                return left.path < right.path;
              }
              return left.fileId < right.fileId;
            });

  std::map<std::uint32_t, std::string> pathByFileId;
  std::set<std::string> knownPaths;
  pathByFileId.clear();
  for (const ai::FileRecord& file : files) {
    pathByFileId[file.fileId] = file.path;
    knownPaths.insert(file.path);
  }

  const std::vector<std::string> lruOrder = lruManager_.snapshot();
  std::map<std::string, std::size_t> hotRankByPath;
  for (std::size_t index = 0U; index < lruOrder.size(); ++index) {
    hotRankByPath.emplace(lruOrder[index], index);
  }

  for (const ai::FileRecord& file : files) {
    memory::StateNode node;
    node.nodeId = fileNodeId(file.path);
    node.nodeType = memory::NodeType::File;
    node.version = snapshotNodeVersion;

    const auto hotRankIt = hotRankByPath.find(file.path);
    const bool isKnownHot =
        hotRankIt != hotRankByPath.end() && hotRankIt->second < kHotWindow;

    node.data["kind"] = "file";
    node.data["path"] = file.path;
    node.data["file_id"] = file.fileId;
    node.data["hash"] = ai::hashToHex(file.hash);
    node.data["weight"] = weightEngine_.weightForPath(file.path);
    node.data["hot_rank"] =
        hotRankIt == hotRankByPath.end()
            ? static_cast<long long>(-1)
            : static_cast<long long>(hotRankIt->second);
    node.data["is_hot"] = isKnownHot;
    newGraph->addNode(node);
  }

  std::map<std::string, std::uint64_t> symbolIdByName;
  for (const ai::SymbolRecord& symbol : state_.symbols) {
    const auto symbolIt = symbolIdByName.find(symbol.name);
    if (symbolIt == symbolIdByName.end() || symbol.symbolId < symbolIt->second) {
      symbolIdByName[symbol.name] = symbol.symbolId;
    }
  }

  std::vector<std::pair<std::string, ai::SymbolNode>> symbolsByName;
  symbolsByName.reserve(state_.symbolIndex.size());
  for (const auto& [name, node] : state_.symbolIndex) {
    symbolsByName.push_back({name, node});
  }
  std::sort(symbolsByName.begin(), symbolsByName.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });

  // Pass real graph dimensions into the cognitive memory layer BEFORE
  // computing the hot slice size. Without this, graphNodeCount_ stays 0
  // and computeCapacity() always returns the 256-node floor regardless of
  // how large the repo is.
  // files.size() + symbolsByName.size() ≈ total graph nodes.
  cognitiveMemoryManager_.setGraphScale(
      state_.files.size() + state_.symbols.size(), 0U);
  const std::size_t hotSliceSize = cognitiveMemoryManager_.governedHotSliceCapacity(
      std::max<std::size_t>(kHotSliceFloor, symbolsByName.size()));
  resetHotSlice(cognitiveMemoryManager_.working, hotSliceSize);

  std::set<std::string> recentFiles;
  for (std::size_t index = 0U;
       index < std::min<std::size_t>(lruOrder.size(), kHotWindow);
       ++index) {
    recentFiles.insert(lruOrder[index]);
  }

  struct SymbolGraphNode {
    memory::StateNode node;
    std::string definedIn;
    std::vector<std::string> usedIn;
  };

  std::vector<SymbolGraphNode> symbolNodes;
  symbolNodes.reserve(symbolsByName.size());
  for (const auto& [name, symbolNode] : symbolsByName) {
    SymbolGraphNode entry;
    entry.node.nodeType = memory::NodeType::Symbol;
    entry.node.version = snapshotNodeVersion;

    const auto idIt = symbolIdByName.find(name);
    const std::uint64_t symbolId =
        idIt == symbolIdByName.end() ? deterministicFallbackSymbolId(name)
                                     : idIt->second;
    entry.node.nodeId = symbolNodeId(symbolId);
    entry.definedIn = symbolNode.definedIn;
    entry.usedIn = sortedUsedIn(symbolNode.usedInFiles);

    const double symbolWeight = entry.definedIn.empty()
                                    ? symbolNode.weight
                                    : weightEngine_.weightForPath(entry.definedIn);

    entry.node.data["kind"] = "symbol";
    entry.node.data["symbol_id"] = symbolId;
    entry.node.data["name"] = symbolNode.name.empty() ? name : symbolNode.name;
    entry.node.data["defined_in"] = entry.definedIn;
    entry.node.data["used_in"] = entry.usedIn;
    entry.node.data["usage_count"] = entry.usedIn.size();
    entry.node.data["weight"] = symbolWeight;
    entry.node.data["centrality"] = symbolNode.centrality;
    entry.node.data["is_hot"] = false;
    entry.node.data["hot_version"] = 0ULL;
    entry.node.data["impact_frequency"] = 0U;

    cognitiveMemoryManager_.working.storeNode(entry.node, globalVersion_);
    if (!entry.definedIn.empty() && recentFiles.find(entry.definedIn) != recentFiles.end()) {
      cognitiveMemoryManager_.working.recordAccess(entry.node.nodeId, globalVersion_);
      cognitiveMemoryManager_.working.promote(entry.node.nodeId, 2.0, globalVersion_);
    }
    for (const std::string& usedFile : entry.usedIn) {
      if (recentFiles.find(usedFile) != recentFiles.end()) {
        cognitiveMemoryManager_.working.recordAccess(entry.node.nodeId, globalVersion_);
      }
    }

    symbolNodes.push_back(std::move(entry));
  }

  const std::vector<memory::StateNode> hotSymbols =
      cognitiveMemoryManager_.working.getTopK(
          std::min<std::size_t>(kMaxHotSymbols, symbolNodes.size()),
          globalVersion_);
  std::set<std::string> hotSymbolIds;
  for (const memory::StateNode& hotSymbol : hotSymbols) {
    hotSymbolIds.insert(hotSymbol.nodeId);
  }

  for (SymbolGraphNode& symbol : symbolNodes) {
    const bool isHot = hotSymbolIds.find(symbol.node.nodeId) != hotSymbolIds.end();
    symbol.node.data["is_hot"] = isHot;
    symbol.node.data["hot_version"] = isHot ? globalVersion_ : 0ULL;
    newGraph->addNode(symbol.node);
  }

  std::vector<std::pair<std::string, std::string>> fileDependencies;
  fileDependencies.reserve(state_.deps.fileEdges.size());
  for (const ai::FileDependencyEdge& edge : state_.deps.fileEdges) {
    const auto fromIt = pathByFileId.find(edge.fromFileId);
    const auto toIt = pathByFileId.find(edge.toFileId);
    if (fromIt == pathByFileId.end() || toIt == pathByFileId.end()) {
      continue;
    }
    fileDependencies.push_back({fromIt->second, toIt->second});
  }
  std::sort(fileDependencies.begin(), fileDependencies.end());
  fileDependencies.erase(
      std::unique(fileDependencies.begin(), fileDependencies.end()),
      fileDependencies.end());

  for (const auto& [fromPath, toPath] : fileDependencies) {
    memory::StateEdge edge;
    edge.edgeId = "dep:" + fromPath + "->" + toPath;
    edge.sourceId = fileNodeId(fromPath);
    edge.targetId = fileNodeId(toPath);
    edge.edgeType = memory::EdgeType::DependsOn;
    newGraph->addEdge(edge);
  }

  for (const SymbolGraphNode& symbol : symbolNodes) {
    if (!symbol.definedIn.empty() && knownPaths.find(symbol.definedIn) != knownPaths.end()) {
      memory::StateEdge edge;
      edge.edgeId = "def:" + symbol.node.nodeId + "->" + fileNodeId(symbol.definedIn);
      edge.sourceId = symbol.node.nodeId;
      edge.targetId = fileNodeId(symbol.definedIn);
      edge.edgeType = memory::EdgeType::Contains;
      newGraph->addEdge(edge);
    }

    for (const std::string& usedInPath : symbol.usedIn) {
      if (knownPaths.find(usedInPath) == knownPaths.end()) {
        continue;
      }
      memory::StateEdge edge;
      edge.edgeId = "use:" + fileNodeId(usedInPath) + "->" + symbol.node.nodeId;
      edge.sourceId = fileNodeId(usedInPath);
      edge.targetId = symbol.node.nodeId;
      edge.edgeType = memory::EdgeType::DependsOn;
      newGraph->addEdge(edge);
    }
  }

  replaceSharedPtr(graph_, std::move(newGraph));
}

}  // namespace ultra::core
