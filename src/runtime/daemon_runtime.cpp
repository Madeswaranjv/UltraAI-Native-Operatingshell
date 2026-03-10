#include "ultra/runtime/daemon_runtime.h"
//deamon_runtime.cpp
#include "ultra/runtime/daemon_health.h"
#include "ultra/runtime/event_types.h"
#include "ultra/runtime/snapshot_store.h"
#include "ultra/runtime/worker_pool.h"

#include "../authority/UltraAuthorityAPI.h"
#include "../ai/FileRegistry.h"
#include "../ai/Hashing.h"
#include "../ai/IntegrityManager.h"
#include "../calibration/PatternDetector.h"
#include "../calibration/UsageTracker.h"
#include "../core/graph_store/GraphLoader.h"
#include "../core/graph_store/GraphStore.h"
#include "../core/state_manager.h"
#include "../engine/query/QueryCache.h"
#include "../engine/scanner.h"
#include "../graph/DependencyGraph.h"
#include "../incremental/IncrementalAnalyzer.h"
#include "../memory/TemporalIndex.h"
#include "../memory/epoch/EpochGuard.h"
#include "../metacognition/MetaCognitiveOrchestrator.h"
#include "../metrics/PerformanceMetrics.h"
#include "../policy_evolution/AdaptivePolicyEngine.h"
#include "change_queue.h"
#include "ContextExtractor.h"
#include "context/ContextSnapshot.h"
#include "file_watcher.h"
#include "ipc_server.h"
#include "query_engine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
//E:\Projects\Ultra\src\runtime\daemon_runtime.cpp
namespace ultra::runtime {

namespace {

constexpr std::uint32_t kRuntimeStateMagic = 0x54525355U;  // "USRT"
constexpr std::uint32_t kRuntimeStateVersion = 1U;
constexpr std::uint32_t kSymbolIndexMagic = 0x58444E49U;   // "INDX"
constexpr std::uint32_t kSymbolIndexVersion = 1U;
constexpr std::size_t kAdaptiveTokenBudgetBase = 512U;
constexpr std::size_t kAdaptiveTokenBudgetMin = 128U;
constexpr std::size_t kAdaptiveTokenBudgetMax = 4096U;
constexpr std::size_t kAdaptiveQueryCacheBase = 256U;
constexpr std::size_t kAdaptiveQueryCacheMin = 64U;
constexpr std::size_t kAdaptiveQueryCacheMax = 1024U;
constexpr std::size_t kAdaptiveHotSliceBase = 256U;
constexpr std::size_t kAdaptiveHotSliceMin = 64U;
constexpr std::size_t kAdaptiveHotSliceMax = 1024U;
constexpr auto kMetaCognitiveInterval = std::chrono::seconds(30);

double clamp01(const double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, 0.0, 1.0);
}

std::size_t scaledBoundedSize(const std::size_t base,
                              const double scale,
                              const std::size_t minValue,
                              const std::size_t maxValue) {
  const double boundedScale = std::clamp(scale, 0.25, 4.0);
  const double scaled = std::round(static_cast<double>(base) * boundedScale);
  if (!std::isfinite(scaled)) {
    return base;
  }
  if (scaled <= static_cast<double>(minValue)) {
    return minValue;
  }
  if (scaled >= static_cast<double>(maxValue)) {
    return maxValue;
  }
  return static_cast<std::size_t>(scaled);
}

std::uint64_t unixTimeMillisNow() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string trimCopy(const std::string& value) {
  std::size_t begin = 0U;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool contextPayloadHasData(const nlohmann::json& contextPayload) {
  if (!contextPayload.is_object()) {
    return false;
  }
  const nlohmann::json nodes =
      contextPayload.value("nodes", nlohmann::json::array());
  const nlohmann::json files =
      contextPayload.value("files", nlohmann::json::array());
  return (nodes.is_array() && !nodes.empty()) ||
         (files.is_array() && !files.empty());
}

void appendContextCandidate(std::vector<std::string>& candidates,
                            std::unordered_set<std::string>& seen,
                            const std::string& rawCandidate) {
  const std::string candidate = trimCopy(rawCandidate);
  if (candidate.empty()) {
    return;
  }
  const std::string key = lowerAscii(candidate);
  if (!seen.insert(key).second) {
    return;
  }
  candidates.push_back(candidate);
}

std::vector<std::string> buildContextCandidates(const std::string& query) {
  std::vector<std::string> candidates;
  std::unordered_set<std::string> seen;
  const std::string trimmedQuery = trimCopy(query);
  appendContextCandidate(candidates, seen, trimmedQuery);

  std::vector<std::string> tokens;
  std::string token;
  for (const unsigned char ch : trimmedQuery) {
    if (std::isalnum(ch) != 0 || ch == '_' || ch == ':' || ch == '/' ||
        ch == '.') {
      token.push_back(static_cast<char>(ch));
      continue;
    }
    if (!token.empty()) {
      tokens.push_back(token);
      token.clear();
    }
  }
  if (!token.empty()) {
    tokens.push_back(token);
  }

  for (const std::string& part : tokens) {
    appendContextCandidate(candidates, seen, part);
    if (!part.empty() &&
        std::islower(static_cast<unsigned char>(part.front())) != 0) {
      std::string capitalized = part;
      capitalized.front() = static_cast<char>(
          std::toupper(static_cast<unsigned char>(capitalized.front())));
      appendContextCandidate(candidates, seen, capitalized);
    }
  }

  if (!tokens.empty()) {
    std::string pascal;
    for (std::string part : tokens) {
      if (part.empty()) {
        continue;
      }
      part.front() = static_cast<char>(
          std::toupper(static_cast<unsigned char>(part.front())));
      pascal += part;
    }
    appendContextCandidate(candidates, seen, pascal);
  }

  const std::string loweredQuery = lowerAscii(trimmedQuery);
  if (loweredQuery.find("incremental") != std::string::npos &&
      loweredQuery.find("analysis") != std::string::npos) {
    appendContextCandidate(candidates, seen, "IncrementalAnalyzer");
  }
  if (loweredQuery.find("graphstore") != std::string::npos ||
      (loweredQuery.find("graph") != std::string::npos &&
       loweredQuery.find("store") != std::string::npos)) {
    appendContextCandidate(candidates, seen, "GraphStore");
  }

  return candidates;
}

template <typename T>
void writePod(std::ofstream& out, const T& value) {
  out.write(reinterpret_cast<const char*>(&value),
            static_cast<std::streamsize>(sizeof(T)));
}

template <typename T>
bool readPod(std::ifstream& in, T& value) {
  in.read(reinterpret_cast<char*>(&value),
          static_cast<std::streamsize>(sizeof(T)));
  return static_cast<bool>(in);
}

bool writeString(std::ofstream& out, const std::string& value) {
  const std::uint32_t len = static_cast<std::uint32_t>(value.size());
  writePod(out, len);
  if (len == 0U) {
    return static_cast<bool>(out);
  }
  out.write(value.data(), static_cast<std::streamsize>(value.size()));
  return static_cast<bool>(out);
}

bool readString(std::ifstream& in, std::string& value) {
  std::uint32_t len = 0U;
  if (!readPod(in, len)) {
    return false;
  }
  value.resize(len);
  if (len == 0U) {
    return true;
  }
  in.read(value.data(), static_cast<std::streamsize>(len));
  return static_cast<bool>(in);
}

IpcResponse makeError(const std::string& message) {
  IpcResponse response;
  response.ok = false;
  response.exitCode = 1;
  response.message = message;
  response.payload = nlohmann::json::object();
  return response;
}

IpcResponse makeOk(const std::string& message,
                   const nlohmann::json& payload = nlohmann::json::object(),
                   const int exitCode = 0) {
  IpcResponse response;
  response.ok = true;
  response.exitCode = exitCode;
  response.message = message;
  response.payload = payload;
  return response;
}

nlohmann::ordered_json authorityRiskToJson(
    const authority::AuthorityRiskReport& report) {
  nlohmann::ordered_json payload;
  payload["score"] = report.score;
  payload["within_threshold"] = report.withinThreshold;
  payload["removed_symbols"] = report.removedSymbols;
  payload["signature_changes"] = report.signatureChanges;
  payload["dependency_breaks"] = report.dependencyBreaks;
  payload["public_api_changes"] = report.publicApiChanges;
  payload["impact_depth"] = report.impactDepth;

  nlohmann::ordered_json diffPayload;
  diffPayload["symbols"] = nlohmann::ordered_json::array();
  for (const diff::semantic::SymbolDiff& symbol : report.diffReport.symbols) {
    nlohmann::ordered_json item;
    item["id"] = symbol.id;
    item["type"] = diff::semantic::toString(symbol.type);
    diffPayload["symbols"].push_back(std::move(item));
  }

  diffPayload["signatures"] = nlohmann::ordered_json::array();
  for (const diff::semantic::SignatureDiff& signature :
       report.diffReport.signatures) {
    nlohmann::ordered_json item;
    item["id"] = signature.id;
    item["change"] = diff::semantic::toString(signature.change);
    diffPayload["signatures"].push_back(std::move(item));
  }

  diffPayload["dependencies"] = nlohmann::ordered_json::array();
  for (const diff::semantic::DependencyDiff& dependency :
       report.diffReport.dependencies) {
    nlohmann::ordered_json item;
    item["from"] = dependency.from;
    item["to"] = dependency.to;
    item["type"] = diff::semantic::toString(dependency.type);
    diffPayload["dependencies"].push_back(std::move(item));
  }

  diffPayload["risk"] = diff::semantic::toString(report.diffReport.overallRisk);
  diffPayload["impactScore"] = report.diffReport.impactScore;
  payload["diff"] = std::move(diffPayload);
  return payload;
}

}  // namespace

struct DaemonRuntime::Impl {
  explicit Impl(std::filesystem::path projectRoot)
      : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                         .lexically_normal()),
        ultraDir_(projectRoot_ / ".ultra"),
        graphDir_(ultraDir_ / "graph"),
        runtimeStatePath_(ultraDir_ / "runtime_state.bin"),
        symbolIndexPath_(ultraDir_ / "symbol_index.bin"),
        contextPrevPath_(projectRoot_ / ".ultra.context.prev.json"),
        contextDiffPath_(projectRoot_ / ".ultra.context-diff.json"),
        scanner_(projectRoot_),
        stateManager_(projectRoot_),
        queryEngine_(projectRoot_),
        queryCache_(kAdaptiveQueryCacheBase),
        fileWatcher_(projectRoot_, changeQueue_),
        ipcServer_(projectRoot_),
        daemonHealth_(projectRoot_),
        graphStore_(graphDir_),
        workerPool_(WorkerPool::recommendedThreadCount()) {}

  bool start(const Options& options, std::string& error) {
    if (running_.load(std::memory_order_acquire)) {
      return true;
    }
    options_ = options;
    std::cout << "[UAIR] workspace root = " << projectRoot_.string() << std::endl;
    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=ensure_dirs\n";
    }

    if (!ensureDirs(error)) {
      if (error.empty()) {
        error = "Failed during ensure_dirs.";
      }
      return false;
    }
    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=bootstrap_state\n";
    }
    ai::RuntimeState bootstrap;
    bootstrap.core.magic = ai::IntegrityManager::kCoreMagic;
    bootstrap.core.schemaVersion = ai::IntegrityManager::kSchemaVersion;
    bootstrap.core.indexVersion = ai::IntegrityManager::kIndexVersion;
    bootstrap.core.runtimeActive = 1U;
    stateManager_.replaceState(std::move(bootstrap));

    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=publish_snapshot\n";
    }
    publishSnapshot();
    bindMemoryToCurrentSnapshot();
    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=ipc_start\n";
    }
    if (!ipcServer_.start(error)) {
      if (error.empty()) {
        error = "Failed during ipc_start.";
      }
      return false;
    }
    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=health_init\n";
    }
    if (!daemonHealth_.initialize(IpcServer::currentProcessId(), error)) {
      ipcServer_.stop();
      if (error.empty()) {
        error = "Failed during health_init.";
      }
      return false;
    }

    running_.store(true, std::memory_order_release);
    shutdownRequested_.store(false, std::memory_order_release);
    stopped_.store(false, std::memory_order_release);
    nextMetaControlTickMs_.store(
        unixTimeMillisNow() +
            static_cast<std::uint64_t>(kMetaCognitiveInterval.count() * 1000ULL),
        std::memory_order_release);

    heartbeatThread_ = std::jthread([this](std::stop_token stopToken) {
      try {
        heartbeatLoop(stopToken);
      } catch (const std::exception& ex) {
        if (options_.verbose) {
          std::cerr << "[UAIR] heartbeat thread terminated: " << ex.what() << '\n';
        }
      } catch (...) {
        if (options_.verbose) {
          std::cerr << "[UAIR] heartbeat thread terminated: unknown failure\n";
        }
      }
    });

    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=fast_load\n";
    }
    if (!loadFast(error)) {
      stop();
      if (error.empty()) {
        error = "Failed during fast_load.";
      }
      return false;
    }

    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=publish_loaded_snapshot\n";
    }
    publishSnapshot();
    bindMemoryToCurrentSnapshot();

    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=worker_pool\n";
    }
    workerPool_.start();

    mutationThread_ = std::jthread([this](std::stop_token stopToken) {
      try {
        mutationLoop(stopToken);
      } catch (const std::exception& ex) {
        if (options_.verbose) {
          std::cerr << "[UAIR] mutation thread terminated: " << ex.what() << '\n';
        }
        running_.store(false, std::memory_order_release);
        changeQueue_.notifyAll();
      } catch (...) {
        if (options_.verbose) {
          std::cerr << "[UAIR] mutation thread terminated: unknown failure\n";
        }
        running_.store(false, std::memory_order_release);
        changeQueue_.notifyAll();
      }
    });

    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=file_watcher\n";
    }
    if (!fileWatcher_.start(error)) {
      stop();
      if (error.empty()) {
        error = "Failed during file_watcher_start.";
      }
      return false;
    }

    if (options_.verbose) {
      std::cout << "[UAIR] daemon start phase=startup_delta_scan_async\n";
    }
    const bool deltaScheduled = workerPool_.submit([this]() {
      std::string deltaError;
      if (!startupDeltaScan(deltaError) && options_.verbose) {
        std::cerr << "[UAIR] startup delta scan skipped: " << deltaError << '\n';
      }
    });
    if (!deltaScheduled) {
      std::string deltaError;
      if (!startupDeltaScan(deltaError) && options_.verbose) {
        std::cerr << "[UAIR] startup delta scan skipped: " << deltaError << '\n';
      }
    }

    runMetaControlLoopOnce();

    if (options_.verbose) {
      std::cout << "[UAIR] daemon start complete\n";
    }
    return true;
  }

  int run(const Options& options, std::string& error) {
    if (!start(options, error)) {
      return 1;
    }

    while (running_.load(std::memory_order_acquire)) {
      std::string requestError;
      const bool ok = ipcServer_.processRequests(
          [this](const IpcRequest& request) { return dispatch(request); }, 24U,
          requestError);
      if (!ok) {
        error = requestError;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    stop();
    return error.empty() ? 0 : 1;
  }

  void requestStop() {
    shutdownRequested_.store(true, std::memory_order_release);
    fileWatcher_.stop();
    changeQueue_.requestShutdown();
    changeQueue_.notifyAll();
  }

  void stop() {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    requestStop();
    running_.store(false, std::memory_order_release);
    fileWatcher_.stop();

    if (mutationThread_.joinable()) {
      mutationThread_.request_stop();
      mutationThread_.join();
    }
    if (heartbeatThread_.joinable()) {
      heartbeatThread_.request_stop();
      heartbeatThread_.join();
    }

    workerPool_.stop();

    std::string persistError;
    (void)persistGraph({}, persistError);
    (void)persistRuntimeState(persistError);
    (void)persistSymbolIndex(persistError);

    ipcServer_.stop();
    daemonHealth_.shutdown();
  }

  [[nodiscard]] bool running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] nlohmann::json statusPayload(const bool verboseStatus) const {
    const core::RuntimeStatusSnapshot status =
        stateManager_.snapshotStatus(changeQueue_.size());
    const GraphSnapshot snapshot =
        snapshotStore_.empty() ? stateManager_.getSnapshot() : snapshotStore_.current();
    const core::MemoryStats memory = stateManager_.getMemoryStats();

    nlohmann::json payload;
    payload["runtime_active"] = status.runtimeActive;
    payload["daemon_pid"] = IpcServer::currentProcessId();
    payload["files_indexed"] = status.filesIndexed;
    payload["symbols_indexed"] = status.symbolsIndexed;
    payload["dependencies_indexed"] = status.dependenciesIndexed;
    payload["pending_changes"] = status.pendingChanges;
    payload["schema_version"] = status.schemaVersion;
    payload["index_version"] = status.indexVersion;
    payload["weights_tracked"] = status.weightsTracked;
    payload["lru_tracked"] = status.lruTracked;
    payload["graph_nodes"] = snapshot.graph ? snapshot.graph->nodeCount() : 0U;
    payload["graph_edges"] = snapshot.graph ? snapshot.graph->edgeCount() : 0U;
    payload["memory_usage_bytes"] = IpcServer::currentProcessMemoryUsageBytes();
    payload["hot_slice_size"] = memory.hotSliceSize;
    payload["snapshot_count"] = memory.snapshotCount;
    payload["active_branch_count"] = memory.activeBranchCount;

    if (verboseStatus) {
      const core::KernelHealthSnapshot health = stateManager_.verifyKernelHealth();
      nlohmann::json healthPayload;
      healthPayload["healthy"] = health.healthy;
      healthPayload["branch_count"] = health.branchCount;
      healthPayload["snapshot_count"] = health.snapshotCount;
      healthPayload["governance_active"] = health.governanceActive;
      healthPayload["determinism_guards_active"] = health.determinismGuardsActive;
      healthPayload["memory_caps_respected"] = health.memoryCapsRespected;
      healthPayload["violations"] = health.violations;
      payload["kernel_health"] = std::move(healthPayload);
    }

    nlohmann::json metaPayload;
    {
      std::scoped_lock lock(metaStateMutex_);
      metaPayload["stability_score"] = lastMetaSignal_.stabilityScore;
      metaPayload["drift_score"] = lastMetaSignal_.driftScore;
      metaPayload["learning_velocity"] = lastMetaSignal_.learningVelocity;
      metaPayload["conservative_mode"] = lastMetaSignal_.enterConservativeMode;
      metaPayload["exploratory_mode"] = lastMetaSignal_.enterExploratoryMode;
      metaPayload["predicted_next_command"] = lastPatternPrediction_;
      metaPayload["last_observed_command"] = lastObservedCommand_;
      metaPayload["last_temporal_category"] = lastTemporalCategory_;
      metaPayload["cache_hit_rate"] = lastCacheHitRate_;
      metaPayload["runtime_signal_score"] = lastRuntimeSignalScore_;
      metaPayload["risk_bias_shift"] = lastPolicyAdjustment_.riskBiasShift;
      metaPayload["determinism_bias_shift"] =
          lastPolicyAdjustment_.determinismBiasShift;
      metaPayload["exploration_bias_shift"] =
          lastPolicyAdjustment_.explorationBiasShift;
      metaPayload["query_token_budget"] =
          adaptiveQueryTokenBudget_.load(std::memory_order_relaxed);
      metaPayload["query_cache_capacity"] =
          activeQueryCacheCapacity_.load(std::memory_order_relaxed);
      metaPayload["hot_slice_capacity"] =
          adaptiveHotSliceCapacity_.load(std::memory_order_relaxed);
      metaPayload["branch_retention_hint"] =
          adaptiveBranchRetentionHint_.load(std::memory_order_relaxed);
      metaPayload["query_count"] =
          queryExecutionCount_.load(std::memory_order_relaxed);
      metaPayload["impact_count"] =
          impactExecutionCount_.load(std::memory_order_relaxed);
      metaPayload["context_generation_count"] =
          contextGenerationCount_.load(std::memory_order_relaxed);
      metaPayload["branch_operation_count"] =
          branchOperationCount_.load(std::memory_order_relaxed);
      metaPayload["commit_operation_count"] =
          commitOperationCount_.load(std::memory_order_relaxed);
      metaPayload["temporal_events_retained"] = temporalEventCount_;
      metaPayload["temporal_recent_window"] = temporalRecentWindowCount_;
      metaPayload["last_control_loop_ms"] = lastMetaControlLoopMs_;
    }
    payload["meta_cognitive"] = std::move(metaPayload);

    return payload;
  }

  void recordUsageEvent(const std::string& command,
                        const std::vector<std::string>& args) {
    std::vector<calibration::UsageEvent> history;
    {
      std::scoped_lock lock(usageMutex_);
      usageTracker_.record(command, args);
      history = usageTracker_.getHistory();
    }

    const std::string predicted =
        patternDetector_.predictNextCommand(history, command);
    std::scoped_lock lock(metaStateMutex_);
    lastObservedCommand_ = command;
    lastPatternPrediction_ = predicted;
  }

  [[nodiscard]] memory::PerformanceSnapshot performanceSnapshotFromReport(
      const nlohmann::ordered_json& report) const {
    memory::PerformanceSnapshot snapshot;
    snapshot.avgTokenSavingsRatio =
        report.value("avg_token_savings_ratio", 0.0);
    snapshot.avgLatencyMs = report.value("avg_latency_ms", 0.0);
    snapshot.errorRate = report.value("error_rate", 0.0);
    snapshot.hotSliceHitRate = report.value("hot_slice_hit_ratio", 0.0);
    snapshot.contextReuseRate = report.value("context_reuse_rate", 0.0);
    snapshot.impactPredictionAccuracy =
        report.value("impact_prediction_accuracy", 0.0);
    const nlohmann::ordered_json context =
        report.value("context", nlohmann::ordered_json::object());
    snapshot.compressionRatio = context.value("compression_ratio", 0.0);
    const nlohmann::ordered_json branch =
        report.value("branch", nlohmann::ordered_json::object());
    snapshot.overlayReuseRate = branch.value("overlay_reuse_rate", 0.0);
    return snapshot;
  }

  void recordTemporalEvent(const std::string& category,
                           const std::string& subject,
                           const GraphSnapshot& snapshot) {
    memory::StateSnapshot eventSnapshot;
    std::size_t retainedCount = 0U;
    std::size_t recentWindow = 0U;
    {
      std::scoped_lock lock(temporalMutex_);
      eventSnapshot.id = ++temporalEventSequence_;
      eventSnapshot.snapshotId = std::to_string(eventSnapshot.id);
      eventSnapshot.nodeCount =
          snapshot.graph ? snapshot.graph->nodeCount() : 0U;
      eventSnapshot.edgeCount =
          snapshot.graph ? snapshot.graph->edgeCount() : 0U;
      eventSnapshot.graphHash =
          category + ":" + subject + ":" + std::to_string(snapshot.version);
      temporalSnapshotChain_.append(eventSnapshot);
      memory::TemporalIndex index(temporalSnapshotChain_);
      const std::uint64_t windowStart =
          eventSnapshot.id > 2U ? eventSnapshot.id - 2U : 0U;
      recentWindow =
          index.getChangesBetween(windowStart, eventSnapshot.id).size();
      retainedCount = temporalSnapshotChain_.size();
    }

    std::scoped_lock lock(metaStateMutex_);
    temporalEventCount_ = retainedCount;
    temporalRecentWindowCount_ = recentWindow;
    lastTemporalCategory_ = category;
  }

  void resizeQueryCache(const std::size_t capacity) {
    const std::size_t bounded = std::clamp(
        capacity, kAdaptiveQueryCacheMin, kAdaptiveQueryCacheMax);
    std::scoped_lock lock(queryCacheMutex_);
    if (activeQueryCacheCapacity_.load(std::memory_order_relaxed) == bounded) {
      return;
    }
    queryCache_ = engine::query::QueryCache(bounded);
    activeQueryCacheCapacity_.store(bounded, std::memory_order_relaxed);
  }

  void applyAdaptiveRuntimePolicies(const GraphSnapshot& snapshot) {
    auto& memoryManager = stateManager_.cognitiveMemory();
    memoryManager.bindToSnapshot(&snapshot);
    const std::size_t governedTokenBudget = std::max<std::size_t>(
        1U, memoryManager.governedTokenBudget(
                adaptiveQueryTokenBudget_.load(std::memory_order_relaxed)));
    adaptiveQueryTokenBudget_.store(governedTokenBudget,
                                    std::memory_order_relaxed);

    const std::size_t governedHotSlice = memoryManager.governedHotSliceCapacity(
        adaptiveHotSliceCapacity_.load(std::memory_order_relaxed));
    memoryManager.working.setMaxSize(governedHotSlice);
    adaptiveHotSliceCapacity_.store(governedHotSlice, std::memory_order_relaxed);

    const core::MemoryStats memory = stateManager_.getMemoryStats();
    memoryManager.applyMemoryGovernance(memory.activeBranchCount, false);
  }

  void runMetaControlLoopOnce() {
    const GraphSnapshot snapshot = snapshotStore_.empty()
                                       ? stateManager_.getSnapshot()
                                       : snapshotStore_.current();
    if (!snapshot.graph) {
      return;
    }

    const nlohmann::ordered_json report = metrics::PerformanceMetrics::report();
    const memory::PerformanceSnapshot perf = performanceSnapshotFromReport(report);

    const std::uint64_t lookups =
        queryCacheLookups_.load(std::memory_order_relaxed);
    const std::uint64_t hits = queryCacheHits_.load(std::memory_order_relaxed);
    const double cacheHitRate =
        lookups == 0U
            ? 0.0
            : static_cast<double>(hits) / static_cast<double>(lookups);
    const double runtimeSignalScore = clamp01(
        (cacheHitRate + perf.hotSliceHitRate + perf.contextReuseRate) / 3.0);

    auto& memoryManager = stateManager_.cognitiveMemory();
    metacognition::MetaCognitiveSignal signal =
        metaOrchestrator_.evaluate(memoryManager.strategic,
                                   memoryManager.episodic);
    signal.stabilityScore =
        clamp01((signal.stabilityScore * 0.65) + (runtimeSignalScore * 0.35));
    signal.driftScore = clamp01(
        (signal.driftScore * 0.70) + ((1.0 - runtimeSignalScore) * 0.30));
    signal.learningVelocity = clamp01(
        (signal.learningVelocity * 0.70) +
        ((perf.impactPredictionAccuracy + cacheHitRate) * 0.15));
    if (runtimeSignalScore < 0.25) {
      signal.enterConservativeMode = true;
      signal.enterExploratoryMode = false;
    } else if (runtimeSignalScore > 0.75 && !signal.enterConservativeMode) {
      signal.enterExploratoryMode = true;
    }
    const policy_evolution::EvolutionAdjustment adjustment =
        adaptivePolicyEngine_.update(signal);

    std::string predictedCommand;
    {
      std::scoped_lock lock(metaStateMutex_);
      predictedCommand = lastPatternPrediction_;
    }

    double tokenScale = 1.0 + adjustment.explorationBiasShift -
                        adjustment.riskBiasShift;
    if (signal.enterConservativeMode) {
      tokenScale -= 0.20;
    } else if (signal.enterExploratoryMode) {
      tokenScale += 0.20;
    }
    if (predictedCommand == "ai_query" ||
        predictedCommand == "authority_context_query") {
      tokenScale += 0.10;
    }
    const std::size_t targetBudget = scaledBoundedSize(
        kAdaptiveTokenBudgetBase, tokenScale, kAdaptiveTokenBudgetMin,
        kAdaptiveTokenBudgetMax);
    adaptiveQueryTokenBudget_.store(
        std::max<std::size_t>(1U, memoryManager.governedTokenBudget(targetBudget)),
        std::memory_order_relaxed);
    std::size_t adaptiveDepth = 2U;
    if (signal.enterConservativeMode || adjustment.riskBiasShift > 0.05) {
      adaptiveDepth = 1U;
    } else if (signal.enterExploratoryMode ||
               adjustment.explorationBiasShift > 0.05) {
      adaptiveDepth = 3U;
    }
    adaptiveImpactDepth_.store(adaptiveDepth, std::memory_order_relaxed);

    double cacheScale = 1.0 + adjustment.explorationBiasShift -
                        adjustment.riskBiasShift;
    if (cacheHitRate < 0.35) {
      cacheScale += 0.20;
    } else if (cacheHitRate > 0.85) {
      cacheScale -= 0.10;
    }
    if (predictedCommand == "ai_query") {
      cacheScale += 0.15;
    }
    resizeQueryCache(scaledBoundedSize(kAdaptiveQueryCacheBase, cacheScale,
                                       kAdaptiveQueryCacheMin,
                                       kAdaptiveQueryCacheMax));

    double hotSliceScale =
        1.0 + adjustment.explorationBiasShift - adjustment.determinismBiasShift;
    if (perf.hotSliceHitRate < 0.45) {
      hotSliceScale += 0.15;
    }
    adaptiveHotSliceCapacity_.store(
        memoryManager.governedHotSliceCapacity(scaledBoundedSize(
            kAdaptiveHotSliceBase, hotSliceScale, kAdaptiveHotSliceMin,
            kAdaptiveHotSliceMax)),
        std::memory_order_relaxed);

    const std::uint64_t branchOps =
        branchOperationCount_.load(std::memory_order_relaxed) +
        commitOperationCount_.load(std::memory_order_relaxed);
    double branchRetentionScale = 1.0 + adjustment.explorationBiasShift;
    if (branchOps > 0U) {
      branchRetentionScale += std::min(
          0.50, static_cast<double>(branchOps) * 0.02);
    }
    adaptiveBranchRetentionHint_.store(
        scaledBoundedSize(3U, branchRetentionScale, 1U, 16U),
        std::memory_order_relaxed);
    recordTemporalEvent("meta_control_loop", predictedCommand, snapshot);

    std::scoped_lock lock(metaStateMutex_);
    lastMetaSignal_ = signal;
    lastPolicyAdjustment_ = adjustment;
    lastCacheHitRate_ = cacheHitRate;
    lastRuntimeSignalScore_ = runtimeSignalScore;
    lastMetaControlLoopMs_ = unixTimeMillisNow();
  }

  void runMetaControlLoopIfDue() {
    const std::uint64_t nowMs = unixTimeMillisNow();
    std::uint64_t dueMs =
        nextMetaControlTickMs_.load(std::memory_order_acquire);
    if (nowMs < dueMs) {
      return;
    }

    const std::uint64_t nextDue =
        nowMs + static_cast<std::uint64_t>(kMetaCognitiveInterval.count() *
                                           1000ULL);
    if (!nextMetaControlTickMs_.compare_exchange_strong(
            dueMs, nextDue, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return;
    }
    runMetaControlLoopOnce();
  }

  [[nodiscard]] IpcResponse dispatch(const IpcRequest& request) {
    runMetaControlLoopIfDue();
    const std::string command =
        request.command.empty() ? request.type : request.command;

    if (command == "ai_status") {
      recordUsageEvent("ai_status", {});
      const bool verboseStatus = request.payload.value("verbose", false);
      return makeOk("status_ready", statusPayload(verboseStatus));
    }

    if (command == "ai_query") {
      const std::string target = request.payload.value("target", "");
      if (target.empty()) {
        return makeError("ai_query requires a non-empty target.");
      }
      recordUsageEvent("ai_query", {target});
      queryExecutionCount_.fetch_add(1U, std::memory_order_relaxed);
      const bool metricsEnabled = metrics::PerformanceMetrics::isEnabled();
      const auto queryStartedAt = std::chrono::steady_clock::now();
      std::size_t snapshotNodeCount = 0U;
      std::size_t snapshotEdgeCount = 0U;
      nlohmann::json out;
      GraphSnapshot snapshotForTemporal;
      bool hasSnapshotForTemporal = false;
      try {
        memory::epoch::EpochGuard guard(memory::epoch::EpochManager::instance());
        const GraphSnapshot snapshot = snapshotStore_.empty()
                                           ? stateManager_.getSnapshot()
                                           : snapshotStore_.current();
        snapshotForTemporal = snapshot;
        hasSnapshotForTemporal = true;
        snapshotNodeCount = snapshot.graph ? snapshot.graph->nodeCount() : 0U;
        snapshotEdgeCount = snapshot.graph ? snapshot.graph->edgeCount() : 0U;
        applyAdaptiveRuntimePolicies(snapshot);

        bool cacheHit = false;
        queryCacheLookups_.fetch_add(1U, std::memory_order_relaxed);
        {
          std::scoped_lock cacheLock(queryCacheMutex_);
          cacheHit = queryCache_.get(target, snapshot.version, out);
          if (!cacheHit) {
            out = queryEngine_.queryTarget(snapshot, target);
            queryCache_.put(target, snapshot.version, out);
          }
        }
        if (cacheHit) {
          queryCacheHits_.fetch_add(1U, std::memory_order_relaxed);
        }
        const std::string kind = out.value("kind", "");
        if (kind == "symbol") {
          recordUsageEvent("ai_query_symbol", {target});
          const auto definitions = stateManager_.findDefinition(target);
          if (!definitions.empty()) {
            stateManager_.cognitiveMemory().working.recordAccess(
                "symbol:" + std::to_string(definitions.front().symbolId),
                snapshot.version);
          }
        } else if (kind == "file") {
          recordUsageEvent("ai_query_file", {out.value("path", target)});
          const nlohmann::json symbolsDefined =
              out.value("symbols_defined", nlohmann::json::array());
          if (symbolsDefined.is_array()) {
            for (const nlohmann::json& item : symbolsDefined) {
              if (!item.is_string()) {
                continue;
              }
              const auto definitions =
                  stateManager_.findDefinition(item.get<std::string>());
              if (definitions.empty()) {
                continue;
              }
              stateManager_.cognitiveMemory().working.recordAccess(
                  "symbol:" + std::to_string(definitions.front().symbolId),
                  snapshot.version);
            }
          }
        }

        if (kind == "symbol" || kind == "file") {
          const std::size_t tokenBudget = std::max<std::size_t>(
              1U,
              request.payload.value(
                  "token_budget",
                  adaptiveQueryTokenBudget_.load(std::memory_order_relaxed)));
          const std::size_t impactDepth = std::max<std::size_t>(
              1U,
              request.payload.value(
                  "impact_depth",
                  adaptiveImpactDepth_.load(std::memory_order_relaxed)));

          Query query;
          query.kind = kind == "symbol" ? QueryKind::Symbol : QueryKind::File;
          query.target = kind == "file" ? out.value("path", target) : target;
          query.impactDepth = impactDepth;

          const CognitiveState state = stateManager_.createCognitiveState(tokenBudget);
          ContextExtractor extractor;
          const ContextSlice context = extractor.getMinimalContext(state, query);
          nlohmann::json aiContext =
              nlohmann::json::parse(context.json, nullptr, false);
          if (aiContext.is_discarded()) {
            aiContext = nlohmann::json::object();
            aiContext["kind"] = kind;
            aiContext["target"] = query.target;
            aiContext["nodes"] = nlohmann::json::array();
            aiContext["files"] = nlohmann::json::array();
            aiContext["impact_region"] = nlohmann::json::array();
            aiContext["metadata"] = nlohmann::json::object(
                {{"estimatedTokens", context.estimatedTokens},
                 {"rawEstimatedTokens", context.estimatedTokens},
                 {"tokenBudget", tokenBudget},
                 {"truncated", false}});
          }
          out["ai_context"] = std::move(aiContext);
          contextGenerationCount_.fetch_add(1U, std::memory_order_relaxed);
          if (hasSnapshotForTemporal) {
            recordTemporalEvent("context_generation", query.target,
                                snapshotForTemporal);
          }
        }
        out["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        if (hasSnapshotForTemporal) {
          recordTemporalEvent("query", target, snapshotForTemporal);
        }
      } catch (const std::exception& ex) {
        return makeError(ex.what());
      }
      if (metricsEnabled) {
        metrics::SnapshotMetrics metric;
        metric.operation = "daemon.ai_query";
        metric.durationMicros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - queryStartedAt)
                .count());
        metric.nodeCount = snapshotNodeCount;
        metric.edgeCount = snapshotEdgeCount;
        metrics::PerformanceMetrics::recordSnapshotMetric(metric);
      }
      const bool found = out.value("kind", "") != "not_found";
      return makeOk(found ? "query_ready" : "target_not_found", out,
                    found ? 0 : 1);
    }

    if (command == "ai_context") {
      const std::string query = trimCopy(
          request.payload.value("query", request.payload.value("target", "")));
      if (query.empty()) {
        return makeError("ai_context requires a non-empty query.");
      }
      recordUsageEvent("ai_context", {query});

      const std::size_t tokenBudget = std::max<std::size_t>(
          1U,
          request.payload.value(
              "token_budget",
              adaptiveQueryTokenBudget_.load(std::memory_order_relaxed)));
      const std::size_t impactDepth = std::max<std::size_t>(
          1U,
          request.payload.value(
              "impact_depth",
              adaptiveImpactDepth_.load(std::memory_order_relaxed)));

      const std::vector<std::string> candidates = buildContextCandidates(query);
      for (const std::string& candidate : candidates) {
        IpcRequest queryRequest;
        queryRequest.command = "ai_query";
        queryRequest.type = "ai_query";
        queryRequest.payload = nlohmann::json::object();
        queryRequest.payload["target"] = candidate;
        queryRequest.payload["token_budget"] = tokenBudget;
        queryRequest.payload["impact_depth"] = impactDepth;

        const IpcResponse queryResponse = dispatch(queryRequest);
        if (!queryResponse.ok) {
          continue;
        }
        const nlohmann::json queryPayload = queryResponse.payload;
        if (queryPayload.value("kind", "") == "not_found") {
          continue;
        }

        const nlohmann::json contextPayload =
            queryPayload.value("ai_context", nlohmann::json::object());
        if (!contextPayloadHasData(contextPayload)) {
          continue;
        }

        nlohmann::ordered_json payload;
        payload["query"] = query;
        payload["resolved_target"] = candidate;
        payload["context"] = contextPayload;
        payload["context_json"] = contextPayload.dump();
        payload["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        return makeOk("context_ready", payload);
      }

      nlohmann::ordered_json payload;
      payload["query"] = query;
      payload["resolved_target"] = "";
      nlohmann::json emptyContext = nlohmann::json::object();
      emptyContext["kind"] = "file_context";
      emptyContext["target"] = query;
      emptyContext["nodes"] = nlohmann::json::array();
      emptyContext["files"] = nlohmann::json::array();
      emptyContext["impact_region"] = nlohmann::json::array();
      emptyContext["metadata"] = nlohmann::json::object(
          {{"estimatedTokens", 0U},
           {"rawEstimatedTokens", 0U},
           {"tokenBudget", tokenBudget},
           {"truncated", false}});
      payload["context"] = emptyContext;
      payload["context_json"] = emptyContext.dump();
      payload["meta_cognitive"] = statusPayload(false).value(
          "meta_cognitive", nlohmann::json::object());
      return makeOk("context_not_found", payload, 1);
    }

    if (command == "ai_source") {
      const std::string target = request.payload.value("file", "");
      if (target.empty()) {
        return makeError("ai_source requires a non-empty file argument.");
      }
      recordUsageEvent("ai_source", {target});

      const GraphSnapshot snapshot = snapshotStore_.empty()
                                         ? stateManager_.getSnapshot()
                                         : snapshotStore_.current();
      const std::string indexedPath =
          queryEngine_.resolveIndexedFilePath(snapshot, target);
      if (indexedPath.empty()) {
        return makeError("Source target is not indexed: " + target);
      }

      nlohmann::json sourcePayload;
      std::string sourceError;
      if (!queryEngine_.readSourceByIndexedPath(indexedPath, sourcePayload,
                                                sourceError)) {
        return makeError(sourceError);
      }
      sourcePayload["meta_cognitive"] = statusPayload(false).value(
          "meta_cognitive", nlohmann::json::object());
      recordTemporalEvent("source_query", target, snapshot);
      return makeOk("source_ready", sourcePayload);
    }

    if (command == "ai_impact") {
      const std::string target = request.payload.value("target", "");
      if (target.empty()) {
        return makeError("ai_impact requires a non-empty target.");
      }
      recordUsageEvent("ai_impact", {target});
      impactExecutionCount_.fetch_add(1U, std::memory_order_relaxed);
      const bool metricsEnabled = metrics::PerformanceMetrics::isEnabled();
      const auto impactStartedAt = std::chrono::steady_clock::now();
      try {
        memory::epoch::EpochGuard guard(memory::epoch::EpochManager::instance());
        const CognitiveState state = stateManager_.createCognitiveState(
            std::numeric_limits<std::size_t>::max());
        applyAdaptiveRuntimePolicies(state.snapshot);
        nlohmann::json out = queryEngine_.queryImpact(state, target);
        const bool found = out.value("kind", "") != "not_found";

        memory::StrategicOutcome outcome;
        outcome.version = state.snapshot.version;
        outcome.category = "impact_prediction";
        outcome.subject = target;
        outcome.success = found;
        outcome.rolledBack = false;
        outcome.predictedRisk = out.value("impact_score", 0.0);
        outcome.observedRisk = outcome.predictedRisk;
        outcome.predictedConfidence = found ? 1.0 : 0.0;
        outcome.observedConfidence = outcome.predictedConfidence;
        stateManager_.cognitiveMemory().strategic.recordOutcome(outcome);

        if (metricsEnabled) {
          metrics::SnapshotMetrics metric;
          metric.operation = "daemon.ai_impact";
          metric.durationMicros = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - impactStartedAt)
                  .count());
          metric.nodeCount =
              state.snapshot.graph ? state.snapshot.graph->nodeCount() : 0U;
          metric.edgeCount =
              state.snapshot.graph ? state.snapshot.graph->edgeCount() : 0U;
          metrics::PerformanceMetrics::recordSnapshotMetric(metric);
          metrics::PerformanceMetrics::recordImpactPredictionAccuracy(
              found ? 1.0 : 0.0);
        }
        out["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        recordTemporalEvent("impact_prediction", target, state.snapshot);
        return makeOk(found ? "impact_ready" : "impact_target_not_found", out,
                      found ? 0 : 1);
      } catch (const std::exception& ex) {
        return makeError(ex.what());
      }
    }

    if (command == "authority_branch_create") {
      authority::AuthorityBranchRequest branchRequest;
      branchRequest.reason = request.payload.value("reason", "");
      branchRequest.parentBranchId =
          request.payload.value("parent_branch_id", "");
      if (branchRequest.reason.empty()) {
        return makeError("branch create requires a non-empty reason.");
      }
      recordUsageEvent("authority_branch_create",
                       {branchRequest.reason, branchRequest.parentBranchId});
      branchOperationCount_.fetch_add(1U, std::memory_order_relaxed);

      try {
        authority::UltraAuthorityAPI authorityApi(projectRoot_);
        const std::string branchId = authorityApi.createBranch(branchRequest);
        const GraphSnapshot snapshot = snapshotStore_.empty()
                                           ? stateManager_.getSnapshot()
                                           : snapshotStore_.current();

        nlohmann::ordered_json payload;
        payload["branch_id"] = branchId;
        payload["parent_branch_id"] = branchRequest.parentBranchId;
        payload["reason"] = branchRequest.reason;
        payload["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        recordTemporalEvent("branch_create", branchId, snapshot);
        return makeOk("branch_created", payload);
      } catch (const std::exception& ex) {
        return makeError(std::string("Branch creation failed: ") + ex.what());
      }
    }

    if (command == "authority_context_query") {
      authority::AuthorityContextRequest contextRequest;
      contextRequest.query = request.payload.value("query", "");
      contextRequest.branchId = request.payload.value("branch_id", "");
      contextRequest.tokenBudget = std::max<std::size_t>(
          1U,
          request.payload.value("token_budget",
                                adaptiveQueryTokenBudget_.load(
                                    std::memory_order_relaxed)));
      contextRequest.impactDepth = std::max<std::size_t>(
          1U,
          request.payload.value("impact_depth",
                                adaptiveImpactDepth_.load(
                                    std::memory_order_relaxed)));
      if (contextRequest.query.empty()) {
        return makeError("context query requires a non-empty query.");
      }
      recordUsageEvent("authority_context_query",
                       {contextRequest.query, contextRequest.branchId});
      contextGenerationCount_.fetch_add(1U, std::memory_order_relaxed);

      try {
        authority::UltraAuthorityAPI authorityApi(projectRoot_);
        const authority::AuthorityContextResult result =
            authorityApi.getContextSlice(contextRequest);
        if (!result.success) {
          return makeError(result.message.empty()
                               ? "Authority context query failed."
                               : result.message);
        }

        nlohmann::json contextPayload =
            nlohmann::json::parse(result.contextJson, nullptr, false);
        if (contextPayload.is_discarded()) {
          contextPayload = nlohmann::json::object();
        }

        if (!contextPayloadHasData(contextPayload)) {
          IpcRequest fallbackRequest;
          fallbackRequest.command = "ai_context";
          fallbackRequest.type = "ai_context";
          fallbackRequest.payload = nlohmann::json::object();
          fallbackRequest.payload["query"] = contextRequest.query;
          fallbackRequest.payload["token_budget"] = contextRequest.tokenBudget;
          fallbackRequest.payload["impact_depth"] = contextRequest.impactDepth;

          IpcResponse fallbackResponse = dispatch(fallbackRequest);
          if (fallbackResponse.ok) {
            return fallbackResponse;
          }
        }

        nlohmann::ordered_json payload;
        payload["context"] = std::move(contextPayload);
        payload["context_json"] = result.contextJson;
        payload["estimated_tokens"] = result.estimatedTokens;
        payload["snapshot_version"] = result.snapshotVersion;
        payload["snapshot_hash"] = result.snapshotHash;
        payload["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        const GraphSnapshot snapshot = snapshotStore_.empty()
                                           ? stateManager_.getSnapshot()
                                           : snapshotStore_.current();
        recordTemporalEvent("context_query", contextRequest.query, snapshot);
        return makeOk("context_ready", payload);
      } catch (const std::exception& ex) {
        return makeError(std::string("Context query failed: ") + ex.what());
      }
    }

    if (command == "authority_intent_simulate") {
      authority::AuthorityIntentRequest intentRequest;
      intentRequest.goal = request.payload.value("goal", "");
      intentRequest.target = request.payload.value("target", "");
      intentRequest.branchId = request.payload.value("branch_id", "");
      intentRequest.tokenBudget = std::max<std::size_t>(
          1U,
          request.payload.value("token_budget",
                                adaptiveQueryTokenBudget_.load(
                                    std::memory_order_relaxed)));
      intentRequest.impactDepth = std::max<std::size_t>(
          1U,
          request.payload.value("impact_depth",
                                adaptiveImpactDepth_.load(
                                    std::memory_order_relaxed)));
      intentRequest.maxFilesChanged = std::max<std::size_t>(
          1U,
          request.payload.value(
              "max_files_changed",
              static_cast<std::size_t>(intentRequest.maxFilesChanged)));
      intentRequest.allowPublicApiChange = request.payload.value(
          "allow_public_api_change", intentRequest.allowPublicApiChange);
      intentRequest.threshold = std::clamp(
          request.payload.value("threshold", intentRequest.threshold), 0.0, 1.0);

      if (intentRequest.target.empty()) {
        intentRequest.target = intentRequest.goal;
      }
      if (intentRequest.goal.empty() && intentRequest.target.empty()) {
        return makeError(
            "intent simulate requires a non-empty goal or --target value.");
      }
      recordUsageEvent("authority_intent_simulate",
                       {intentRequest.goal, intentRequest.target});

      try {
        authority::UltraAuthorityAPI authorityApi(projectRoot_);
        const authority::AuthorityRiskReport report =
            authorityApi.evaluateRisk(intentRequest);
        nlohmann::ordered_json payload = authorityRiskToJson(report);
        payload["threshold"] = intentRequest.threshold;
        payload["goal"] = intentRequest.goal;
        payload["target"] = intentRequest.target;
        payload["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        const GraphSnapshot snapshot = snapshotStore_.empty()
                                           ? stateManager_.getSnapshot()
                                           : snapshotStore_.current();
        recordTemporalEvent("intent_simulation", intentRequest.target, snapshot);
        return makeOk(
            report.withinThreshold ? "intent_simulation_ready"
                                   : "intent_simulation_threshold_exceeded",
            payload, report.withinThreshold ? 0 : 1);
      } catch (const std::exception& ex) {
        return makeError(std::string("Intent simulation failed: ") + ex.what());
      }
    }

    if (command == "authority_commit") {
      authority::AuthorityCommitRequest commitRequest;
      commitRequest.sourceBranchId = request.payload.value("source_branch_id", "");
      commitRequest.targetBranchId =
          request.payload.value("target_branch_id", commitRequest.targetBranchId);
      commitRequest.maxAllowedRisk = std::clamp(
          request.payload.value("max_allowed_risk", commitRequest.maxAllowedRisk),
          0.0, 1.0);
      commitRequest.policy.maxImpactDepth = static_cast<int>(
          adaptiveImpactDepth_.load(std::memory_order_relaxed));
      commitRequest.policy.maxTokenBudget = static_cast<int>(
          adaptiveQueryTokenBudget_.load(std::memory_order_relaxed));

      const nlohmann::json policyPayload =
          request.payload.value("policy", nlohmann::json::object());
      if (policyPayload.is_object()) {
        commitRequest.policy.maxImpactDepth = policyPayload.value(
            "max_impact_depth", commitRequest.policy.maxImpactDepth);
        commitRequest.policy.maxFilesChanged = policyPayload.value(
            "max_files_changed", commitRequest.policy.maxFilesChanged);
        commitRequest.policy.maxTokenBudget = policyPayload.value(
            "max_token_budget", commitRequest.policy.maxTokenBudget);
        commitRequest.policy.allowPublicAPIChange = policyPayload.value(
            "allow_public_api_change",
            commitRequest.policy.allowPublicAPIChange);
        commitRequest.policy.allowCrossModuleMove = policyPayload.value(
            "allow_cross_module_move",
            commitRequest.policy.allowCrossModuleMove);
        commitRequest.policy.requireDeterminism = policyPayload.value(
            "require_determinism", commitRequest.policy.requireDeterminism);
      }

      if (commitRequest.sourceBranchId.empty()) {
        return makeError("commit requires non-empty source branch ID.");
      }
      recordUsageEvent("authority_commit",
                       {commitRequest.sourceBranchId, commitRequest.targetBranchId});
      commitOperationCount_.fetch_add(1U, std::memory_order_relaxed);

      try {
        authority::UltraAuthorityAPI authorityApi(projectRoot_);
        std::string commitError;
        const bool committed =
            authorityApi.commitWithPolicy(commitRequest, commitError);
        if (!committed) {
          return makeError(commitError.empty()
                               ? "Commit rejected by authority policy."
                               : commitError);
        }

        nlohmann::ordered_json payload;
        payload["source"] = commitRequest.sourceBranchId;
        payload["target"] = commitRequest.targetBranchId;
        payload["max_risk"] = commitRequest.maxAllowedRisk;
        payload["status"] = "committed";
        payload["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        const GraphSnapshot snapshot = snapshotStore_.empty()
                                           ? stateManager_.getSnapshot()
                                           : snapshotStore_.current();
        recordTemporalEvent("commit", commitRequest.sourceBranchId, snapshot);
        return makeOk("commit_ready", payload);
      } catch (const std::exception& ex) {
        return makeError(std::string("Commit failed: ") + ex.what());
      }
    }

    if (command == "authority_savings") {
      recordUsageEvent("authority_savings", {});
      try {
        authority::UltraAuthorityAPI authorityApi(projectRoot_);
        nlohmann::json payload = authorityApi.getSavingsReport();
        payload["meta_cognitive"] = statusPayload(false).value(
            "meta_cognitive", nlohmann::json::object());
        return makeOk("savings_ready", payload);
      } catch (const std::exception& ex) {
        return makeError(std::string("Savings report failed: ") + ex.what());
      }
    }

    if (command == "ai_metrics") {
      recordUsageEvent("ai_metrics", {});
      const std::string action = request.payload.value("action", "report");
      if (action == "enable") {
        metrics::PerformanceMetrics::setEnabled(true);
      } else if (action == "disable") {
        metrics::PerformanceMetrics::setEnabled(false);
      } else if (action == "reset") {
        metrics::PerformanceMetrics::reset();
      } else if (action != "report") {
        return makeError(
            "ai_metrics action must be one of: report, enable, disable, reset.");
      }

      nlohmann::json payload;
      payload["action"] = action;
      payload["report"] = metrics::PerformanceMetrics::report();
      payload["meta_cognitive"] = statusPayload(false).value(
          "meta_cognitive", nlohmann::json::object());
      return makeOk("metrics_ready", payload);
    }

    if (command == "rebuild_ai") {
      recordUsageEvent("rebuild_ai", {});
      changeQueue_.requestRebuild();
      return makeOk("rebuild_enqueued");
    }

    if (command == "context_diff") {
      recordUsageEvent("context_diff", {});
      nlohmann::json payload;
      std::string diffError;
      if (!contextDiff(payload, diffError)) {
        return makeError(diffError.empty() ? "context_diff failed." : diffError);
      }
      return makeOk("context_diff_ready", payload);
    }

    if (command == "sleep_ai") {
      recordUsageEvent("sleep_ai", {});
      requestStop();
      return makeOk("shutdown_requested");
    }

    return makeError("Unknown command: " + command);
  }

  bool contextDiff(nlohmann::json& payloadOut, std::string& error) {
    const ai::RuntimeState state = stateManager_.snapshotState();
    std::unordered_map<std::string, std::string> current;
    current.reserve(state.files.size());
    for (const ai::FileRecord& file : state.files) {
      current[file.path] = ai::hashToHex(file.hash);
    }

    const std::unordered_map<std::string, std::string> previous =
        context::loadSnapshot(contextPrevPath_);

    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> modified;
    for (const auto& [path, hash] : current) {
      const auto it = previous.find(path);
      if (it == previous.end()) {
        added.push_back(path);
      } else if (it->second != hash) {
        modified.push_back(path);
      }
    }
    for (const auto& [path, hash] : previous) {
      (void)hash;
      if (current.find(path) == current.end()) {
        removed.push_back(path);
      }
    }

    auto sortUnique = [](std::vector<std::string>& values) {
      std::sort(values.begin(), values.end());
      values.erase(std::unique(values.begin(), values.end()), values.end());
    };
    sortUnique(added);
    sortUnique(removed);
    sortUnique(modified);

    std::set<std::string> changedSet;
    changedSet.insert(added.begin(), added.end());
    changedSet.insert(removed.begin(), removed.end());
    changedSet.insert(modified.begin(), modified.end());
    std::vector<std::string> changed(changedSet.begin(), changedSet.end());

    payloadOut = nlohmann::json::object();
    payloadOut["added"] = added;
    payloadOut["removed"] = removed;
    payloadOut["modified"] = modified;
    payloadOut["changed"] = changed;
    payloadOut["affected"] = changed;
    payloadOut["generated_at_ms"] = unixTimeMillisNow();

    try {
      std::ofstream out(contextDiffPath_, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "Failed to write context diff output: " + contextDiffPath_.string();
        return false;
      }
      out << payloadOut.dump(2);
      out.flush();
      if (!out) {
        error = "Failed to flush context diff output: " + contextDiffPath_.string();
        return false;
      }
    } catch (const std::exception& ex) {
      error = std::string("Failed to persist context diff payload: ") + ex.what();
      return false;
    }

    context::saveSnapshot(contextPrevPath_, current);
    return true;
  }

 private:
  bool ensureDirs(std::string& error) const {
    try {
      std::cout << "[UAIR] creating workspace directories" << std::endl;
      std::filesystem::path ultraRoot = projectRoot_ / ".ultra";
      const std::array<std::filesystem::path, 7U> directories = {
          ultraRoot,
          ultraRoot / "runtime",
          ultraRoot / "runtime" / "ipc",
          ultraRoot / "graph",
          ultraRoot / "index",
          ultraRoot / "context",
          ultraRoot / "memory"};
      for (const std::filesystem::path& dir : directories) {
        std::error_code ec;
        const bool created = std::filesystem::create_directories(dir, ec);
        if (ec) {
          error = "Failed to create workspace directory '" + dir.string() +
                  "': " + ec.message();
          return false;
        }
        if (!std::filesystem::exists(dir)) {
          error = "Workspace directory missing after creation: " + dir.string();
          return false;
        }
        std::cout << "[UAIR] "
                  << (created ? "created directory: " : "directory exists: ")
                  << dir.string() << std::endl;
      }
      return true;
    } catch (const std::exception& ex) {
      error = std::string("Failed to prepare daemon runtime directories: ") +
              ex.what();
      return false;
    }
  }

  bool loadFast(std::string& error) {
    ai::RuntimeState loaded;
    std::string graphError;
    if (graphStore_.load(loaded, graphError)) {
      core::graph_store::GraphLoader::normalizeRuntimeState(loaded);
      core::graph_store::GraphLoader::rebuildSymbolIndex(loaded);
      loaded.core.magic = ai::IntegrityManager::kCoreMagic;
      loaded.core.schemaVersion = ai::IntegrityManager::kSchemaVersion;
      loaded.core.indexVersion = ai::IntegrityManager::kIndexVersion;
      loaded.core.runtimeActive = 1U;
      stateManager_.replaceState(std::move(loaded));
      return true;
    }

    std::string persistedError;
    if (stateManager_.loadPersistedGraph(persistedError)) {
      ai::RuntimeState state = stateManager_.snapshotState();
      state.core.magic = ai::IntegrityManager::kCoreMagic;
      state.core.schemaVersion = ai::IntegrityManager::kSchemaVersion;
      state.core.indexVersion = ai::IntegrityManager::kIndexVersion;
      state.core.runtimeActive = 1U;
      stateManager_.replaceState(std::move(state));
      (void)persistGraph({}, graphError);
      (void)persistRuntimeState(graphError);
      (void)persistSymbolIndex(graphError);
      return true;
    }

    if (options_.verbose) {
      std::cerr << "[UAIR] fast-load miss; starting with empty snapshot.\n";
      if (!graphError.empty()) {
        std::cerr << "[UAIR] graph error: " << graphError << '\n';
      }
      if (!persistedError.empty()) {
        std::cerr << "[UAIR] persisted error: " << persistedError << '\n';
      }
    }

    ai::RuntimeState empty;
    empty.core.magic = ai::IntegrityManager::kCoreMagic;
    empty.core.schemaVersion = ai::IntegrityManager::kSchemaVersion;
    empty.core.indexVersion = ai::IntegrityManager::kIndexVersion;
    empty.core.runtimeActive = 1U;
    stateManager_.replaceState(std::move(empty));
    error.clear();
    return true;
  }

  bool fullRebuild(std::string& error) {
    engine::ScanOutput output;
    if (!scanner_.fullScanParallel(output, error)) {
      return false;
    }

    ai::RuntimeState state;
    state.files = std::move(output.files);
    state.symbols = std::move(output.symbols);
    state.deps = std::move(output.deps);
    state.semanticSymbolDepsByFileId = std::move(output.semanticSymbolDepsByFileId);
    state.symbolIndex = std::move(output.symbolIndex);
    state.core.magic = ai::IntegrityManager::kCoreMagic;
    state.core.schemaVersion = ai::IntegrityManager::kSchemaVersion;
    state.core.indexVersion = ai::IntegrityManager::kIndexVersion;
    state.core.runtimeActive = 1U;

    stateManager_.replaceState(std::move(state));
    publishSnapshot();
    bindMemoryToCurrentSnapshot();

    if (!persistGraph({}, error)) {
      return false;
    }
    if (!persistRuntimeState(error)) {
      return false;
    }
    if (!persistSymbolIndex(error)) {
      return false;
    }
    return true;
  }

  [[nodiscard]] graph::DependencyGraph dependencyGraphFromState(
      const ai::RuntimeState& state) const {
    graph::DependencyGraph graph;
    std::map<std::uint32_t, std::string> pathByFileId;
    for (const ai::FileRecord& file : state.files) {
      pathByFileId[file.fileId] = file.path;
      graph.addNode(file.path);
    }
    for (const ai::FileDependencyEdge& edge : state.deps.fileEdges) {
      const auto fromIt = pathByFileId.find(edge.fromFileId);
      const auto toIt = pathByFileId.find(edge.toFileId);
      if (fromIt == pathByFileId.end() || toIt == pathByFileId.end()) {
        continue;
      }
      graph.addDependency(fromIt->second, toIt->second);
    }
    return graph;
  }

  [[nodiscard]] std::vector<std::string> rebuildSetForChange(
      const ai::RuntimeState& state,
      const std::string& changedPath) const {
    if (changedPath.empty()) {
      return {};
    }
    const graph::DependencyGraph graph = dependencyGraphFromState(state);
    return incremental::IncrementalAnalyzer::computeRebuildSet({changedPath},
                                                               graph);
  }

  static void mergeScanOutput(engine::ScanOutput& aggregate,
                              const engine::ScanOutput& delta) {
    aggregate.changeSet.added.insert(delta.changeSet.added.begin(),
                                     delta.changeSet.added.end());
    aggregate.changeSet.modified.insert(delta.changeSet.modified.begin(),
                                        delta.changeSet.modified.end());
    aggregate.changeSet.deleted.insert(delta.changeSet.deleted.begin(),
                                       delta.changeSet.deleted.end());
    aggregate.changesForLog.insert(aggregate.changesForLog.end(),
                                   delta.changesForLog.begin(),
                                   delta.changesForLog.end());
  }

  bool persistGraph(const std::vector<std::uint32_t>& touchedFileIds,
                    std::string& error)  {
    const ai::RuntimeState state = stateManager_.snapshotState();
    if (touchedFileIds.empty()) {
      return graphStore_.persistFull(state, error);
    }
    return graphStore_.applyIncremental(state, touchedFileIds, error);
  }

  bool persistRuntimeState(std::string& error) const {
    const ai::RuntimeState state = stateManager_.snapshotState();

    try {
      const std::filesystem::path tmpPath = runtimeStatePath_.string() + ".tmp";
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "Failed to open runtime state file: " + tmpPath.string();
        return false;
      }

      writePod(out, kRuntimeStateMagic);
      writePod(out, kRuntimeStateVersion);
      const std::uint32_t count = static_cast<std::uint32_t>(state.files.size());
      writePod(out, count);

      std::vector<const ai::FileRecord*> ordered;
      ordered.reserve(state.files.size());
      for (const ai::FileRecord& record : std::span<const ai::FileRecord>(state.files)) {
        ordered.push_back(&record);
      }
      std::sort(ordered.begin(), ordered.end(),
                [](const ai::FileRecord* left, const ai::FileRecord* right) {
                  if (left->path != right->path) {
                    return left->path < right->path;
                  }
                  return left->fileId < right->fileId;
                });

      for (const ai::FileRecord* record : ordered) {
        if (!writeString(out, record->path)) {
          error = "Failed to write runtime file path entry.";
          return false;
        }
        writePod(out, record->lastModified);
      }

      out.flush();
      if (!out) {
        error = "Failed to flush runtime state file: " + tmpPath.string();
        return false;
      }
      out.close();

      std::error_code ec;
      std::filesystem::remove(runtimeStatePath_, ec);
      std::filesystem::rename(tmpPath, runtimeStatePath_);
      return true;
    } catch (const std::exception& ex) {
      error = std::string("Failed to persist runtime state: ") + ex.what();
      return false;
    }
  }

  bool persistSymbolIndex(std::string& error) const {
    const ai::RuntimeState state = stateManager_.snapshotState();

    try {
      const std::filesystem::path tmpPath = symbolIndexPath_.string() + ".tmp";
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        error = "Failed to open symbol index file: " + tmpPath.string();
        return false;
      }

      writePod(out, kSymbolIndexMagic);
      writePod(out, kSymbolIndexVersion);
      const std::uint32_t count =
          static_cast<std::uint32_t>(state.symbolIndex.size());
      writePod(out, count);

      std::vector<std::string> names;
      names.reserve(state.symbolIndex.size());
      for (const auto& [name, node] : state.symbolIndex) {
        (void)node;
        names.push_back(name);
      }
      std::sort(names.begin(), names.end());
      for (const std::string& name : names) {
        if (!writeString(out, name)) {
          error = "Failed to write symbol index entry.";
          return false;
        }
      }

      out.flush();
      if (!out) {
        error = "Failed to flush symbol index file: " + tmpPath.string();
        return false;
      }
      out.close();

      std::error_code ec;
      std::filesystem::remove(symbolIndexPath_, ec);
      std::filesystem::rename(tmpPath, symbolIndexPath_);
      return true;
    } catch (const std::exception& ex) {
      error = std::string("Failed to persist symbol index: ") + ex.what();
      return false;
    }
  }

  bool startupDeltaScan(std::string& error) {
    std::map<std::string, std::uint64_t> previous;
    if (std::filesystem::exists(runtimeStatePath_)) {
      std::ifstream in(runtimeStatePath_, std::ios::binary);
      if (in) {
        std::uint32_t magic = 0U;
        std::uint32_t version = 0U;
        std::uint32_t count = 0U;
        if (readPod(in, magic) && readPod(in, version) && readPod(in, count) &&
            magic == kRuntimeStateMagic && version == kRuntimeStateVersion) {
          for (std::uint32_t idx = 0U; idx < count; ++idx) {
            std::string path;
            std::uint64_t modified = 0U;
            if (!readString(in, path) || !readPod(in, modified)) {
              previous.clear();
              break;
            }
            previous[path] = modified;
          }
        }
      }
    }

    if (previous.empty()) {
      const ai::RuntimeState state = stateManager_.snapshotState();
      for (const ai::FileRecord& file : state.files) {
        previous[file.path] = file.lastModified;
      }
    }

    const std::vector<ai::DiscoveredFile> discovered =
        ai::FileRegistry::discoverProjectFiles(projectRoot_);

    std::map<std::string, std::uint64_t> current;
    for (const ai::DiscoveredFile& file : discovered) {
      current[file.relativePath] = file.lastModified;
      const auto it = previous.find(file.relativePath);
      if (it == previous.end()) {
        changeQueue_.push(ChangeEvent{ChangeType::Added, file.relativePath});
      } else if (it->second != file.lastModified) {
        changeQueue_.push(ChangeEvent{ChangeType::Modified, file.relativePath});
      }
    }

    for (const auto& [path, modified] : previous) {
      (void)modified;
      if (current.find(path) == current.end()) {
        changeQueue_.push(ChangeEvent{ChangeType::Removed, path});
      }
    }

    (void)error;
    return true;
  }

  void publishSnapshot() {
    snapshotStore_.publish(stateManager_.getSnapshot());
  }

  void bindMemoryToCurrentSnapshot() {
    const GraphSnapshot snapshot = snapshotStore_.empty()
                                       ? stateManager_.getSnapshot()
                                       : snapshotStore_.current();
    stateManager_.cognitiveMemory().bindToSnapshot(&snapshot);
  }

  void mutationLoop(const std::stop_token stopToken) {
    bool sawShutdown = false;
    while (!stopToken.stop_requested()) {
      DaemonEvent event;
      if (!changeQueue_.popRuntimeEvent(event, stopToken,
                                        std::chrono::milliseconds(150))) {
        if (sawShutdown && changeQueue_.size() == 0U) {
          break;
        }
        continue;
      }

      if (event.type == DaemonEventType::ShutdownRequest) {
        sawShutdown = true;
        shutdownRequested_.store(true, std::memory_order_release);
        if (changeQueue_.size() == 0U) {
          break;
        }
        continue;
      }

      try {
        std::string mutationError;
        if (!applyMutation(event, mutationError) && options_.verbose) {
          std::cerr << "[UAIR] mutation failed: " << mutationError << '\n';
        }
      } catch (const std::exception& ex) {
        if (options_.verbose) {
          std::cerr << "[UAIR] mutation exception: " << ex.what() << '\n';
        }
      } catch (...) {
        if (options_.verbose) {
          std::cerr << "[UAIR] mutation exception: unknown failure\n";
        }
      }

      if (sawShutdown && changeQueue_.size() == 0U) {
        break;
      }
    }

    running_.store(false, std::memory_order_release);
    changeQueue_.notifyAll();
  }

  bool applyMutation(const DaemonEvent& event, std::string& error) {
    try {
      if (event.type == DaemonEventType::RebuildRequest) {
        return fullRebuild(error);
      }

      engine::ScanOutput output;
      std::string scanError;
      const bool applied = stateManager_.withWriteLock(
          [&](ai::RuntimeState& state, engine::WeightEngine& weights,
              memory::LruManager& lru) {
            const std::vector<std::string> rebuildSet =
                rebuildSetForChange(state, event.path);
            bool ok = false;
            switch (event.type) {
              case DaemonEventType::FileAdded:
                ok = scanner_.incrementalAdd(state, event.path, output, scanError);
                if (ok && !output.changeSet.added.empty()) {
                  weights.incrementalAdd(event.path);
                  lru.touch(event.path);
                }
                break;
              case DaemonEventType::FileModified:
                ok =
                    scanner_.incrementalModify(state, event.path, output, scanError);
                if (ok && !output.changeSet.modified.empty()) {
                  weights.incrementalModify(event.path);
                  lru.touch(event.path);
                }
                break;
              case DaemonEventType::FileRemoved:
                ok = scanner_.incrementalRemove(state, event.path, output, scanError);
                if (ok && !output.changeSet.deleted.empty()) {
                  weights.incrementalRemove(event.path);
                  lru.erase(event.path);
                }
                break;
              case DaemonEventType::RebuildRequest:
              case DaemonEventType::ShutdownRequest:
                ok = true;
                break;
            }
            if (!ok) {
              return false;
            }

            for (const std::string& rebuildPath : rebuildSet) {
              if (rebuildPath.empty() || rebuildPath == event.path) {
                continue;
              }
              engine::ScanOutput rebuildOutput;
              if (!scanner_.incrementalModify(state, rebuildPath, rebuildOutput,
                                              scanError)) {
                return false;
              }
              if (rebuildOutput.changeSet.empty()) {
                continue;
              }
              if (!rebuildOutput.changeSet.added.empty() ||
                  !rebuildOutput.changeSet.modified.empty()) {
                weights.incrementalModify(rebuildPath);
                lru.touch(rebuildPath);
              }
              if (!rebuildOutput.changeSet.deleted.empty()) {
                weights.incrementalRemove(rebuildPath);
                lru.erase(rebuildPath);
              }
              mergeScanOutput(output, rebuildOutput);
            }
            return ok;
          });

      if (!applied) {
        error = scanError.empty() ? "Incremental mutation failed." : scanError;
        return false;
      }
      if (output.changeSet.empty()) {
        return true;
      }

      std::vector<std::uint32_t> touched;
      touched.reserve(output.changesForLog.size());
      for (const ai::ChangeLogRecord& change : output.changesForLog) {
        if (change.fileId != 0U) {
          touched.push_back(change.fileId);
        }
      }
      std::sort(touched.begin(), touched.end());
      touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

      if (!persistGraph(touched, error)) {
        return false;
      }
      if (!persistRuntimeState(error)) {
        return false;
      }
      if (!persistSymbolIndex(error)) {
        return false;
      }

      publishSnapshot();
      bindMemoryToCurrentSnapshot();
      const GraphSnapshot snapshot = snapshotStore_.current();
      if (snapshot.graph) {
        recordTemporalEvent("graph_mutation", event.path, snapshot);
      }
      scheduleTasks(event.path);
      return true;
    } catch (const std::exception& ex) {
      error = std::string("Incremental mutation threw an exception: ") + ex.what();
      return false;
    } catch (...) {
      error = "Incremental mutation threw an unknown exception.";
      return false;
    }
  }

  void scheduleTasks(const std::string& path) {
    if (path.empty()) {
      return;
    }

    const std::filesystem::path absolute = (projectRoot_ / path).lexically_normal();
    (void)workerPool_.submit([absolute]() {
      std::string hashError;
      ai::Sha256Hash hash{};
      (void)ai::sha256OfFile(absolute, hash, hashError);
    });

    (void)workerPool_.submit([this, path]() {
      const GraphSnapshot snapshot = snapshotStore_.current();
      if (!snapshot.graph) {
        return;
      }
      (void)queryEngine_.resolveIndexedFilePath(snapshot, path);
    });

    const std::string symbolHint = std::filesystem::path(path).stem().string();
    if (!symbolHint.empty()) {
      (void)workerPool_.submit([this, symbolHint]() {
        (void)stateManager_.findSymbolDependencies(symbolHint);
      });
      (void)workerPool_.submit([this, symbolHint]() {
        (void)stateManager_.findImpactRegion(symbolHint, 2U);
      });
    }
  }

  void heartbeatLoop(const std::stop_token stopToken) {
    while (!stopToken.stop_requested() &&
           running_.load(std::memory_order_acquire)) {
      try {
        const core::RuntimeStatusSnapshot status =
            stateManager_.snapshotStatus(changeQueue_.size());
        const GraphSnapshot snapshot = snapshotStore_.empty()
                                           ? stateManager_.getSnapshot()
                                           : snapshotStore_.current();
        const core::MemoryStats memory = stateManager_.getMemoryStats();

        DaemonHeartbeatSnapshot heartbeat;
        heartbeat.runtimeActive = status.runtimeActive;
        heartbeat.lastHeartbeatMs = unixTimeMillisNow();
        heartbeat.indexedFiles = status.filesIndexed;
        heartbeat.symbolsIndexed = status.symbolsIndexed;
        heartbeat.dependenciesIndexed = status.dependenciesIndexed;
        heartbeat.graphNodes = snapshot.graph ? snapshot.graph->nodeCount() : 0U;
        heartbeat.graphEdges = snapshot.graph ? snapshot.graph->edgeCount() : 0U;
        heartbeat.pendingChanges = status.pendingChanges;
        heartbeat.hotSliceSize = memory.hotSliceSize;
        heartbeat.snapshotCount = memory.snapshotCount;
        heartbeat.activeBranchCount = memory.activeBranchCount;
        heartbeat.memoryUsageBytes = IpcServer::currentProcessMemoryUsageBytes();

        std::string hbError;
        (void)daemonHealth_.writeHeartbeat(IpcServer::currentProcessId(), heartbeat,
                                           hbError);

        DaemonHeartbeat legacy;
        legacy.runtimeActive = heartbeat.runtimeActive;
        legacy.lastHeartbeat = heartbeat.lastHeartbeatMs;
        legacy.indexedFiles = heartbeat.indexedFiles;
        legacy.symbolsIndexed = heartbeat.symbolsIndexed;
        legacy.dependenciesIndexed = heartbeat.dependenciesIndexed;
        legacy.graphNodes = heartbeat.graphNodes;
        legacy.graphEdges = heartbeat.graphEdges;
        legacy.pendingChanges = heartbeat.pendingChanges;
        legacy.hotSliceSize = heartbeat.hotSliceSize;
        legacy.snapshotCount = heartbeat.snapshotCount;
        legacy.activeBranchCount = heartbeat.activeBranchCount;
        (void)ipcServer_.writeHeartbeat(legacy, hbError);

        runMetaControlLoopIfDue();
      } catch (const std::exception& ex) {
        if (options_.verbose) {
          std::cerr << "[UAIR] heartbeat update failed: " << ex.what() << '\n';
        }
      } catch (...) {
        if (options_.verbose) {
          std::cerr << "[UAIR] heartbeat update failed: unknown failure\n";
        }
      }

      const auto interval = DaemonHealth::heartbeatInterval();
      const auto step = std::chrono::milliseconds(200);
      for (auto elapsed = std::chrono::milliseconds(0);
           elapsed < interval && !stopToken.stop_requested(); elapsed += step) {
        std::this_thread::sleep_for(step);
      }
    }
  }

  std::filesystem::path projectRoot_;
  std::filesystem::path ultraDir_;
  std::filesystem::path graphDir_;
  std::filesystem::path runtimeStatePath_;
  std::filesystem::path symbolIndexPath_;
  std::filesystem::path contextPrevPath_;
  std::filesystem::path contextDiffPath_;

  engine::Scanner scanner_;
  core::StateManager stateManager_;
  QueryEngine queryEngine_;
  engine::query::QueryCache queryCache_;
  ChangeQueue changeQueue_;
  FileWatcher fileWatcher_;
  IpcServer ipcServer_;
  DaemonHealth daemonHealth_;
  core::graph_store::GraphStore graphStore_;
  SnapshotStore snapshotStore_;
  WorkerPool workerPool_;
  metacognition::MetaCognitiveOrchestrator metaOrchestrator_;
  policy_evolution::AdaptivePolicyEngine adaptivePolicyEngine_;
  calibration::PatternDetector patternDetector_;
  calibration::UsageTracker usageTracker_;
  memory::SnapshotChain temporalSnapshotChain_;

  mutable std::mutex metaStateMutex_;
  mutable std::mutex queryCacheMutex_;
  mutable std::mutex usageMutex_;
  mutable std::mutex temporalMutex_;

  std::jthread mutationThread_;
  std::jthread heartbeatThread_;

  std::atomic<bool> running_{false};
  std::atomic<bool> shutdownRequested_{false};
  std::atomic<bool> stopped_{true};
  std::atomic<std::size_t> adaptiveQueryTokenBudget_{kAdaptiveTokenBudgetBase};
  std::atomic<std::size_t> adaptiveImpactDepth_{2U};
  std::atomic<std::size_t> adaptiveHotSliceCapacity_{kAdaptiveHotSliceBase};
  std::atomic<std::size_t> adaptiveBranchRetentionHint_{3U};
  std::atomic<std::size_t> activeQueryCacheCapacity_{kAdaptiveQueryCacheBase};
  std::atomic<std::uint64_t> nextMetaControlTickMs_{0U};
  std::atomic<std::uint64_t> queryCacheHits_{0U};
  std::atomic<std::uint64_t> queryCacheLookups_{0U};
  std::atomic<std::uint64_t> queryExecutionCount_{0U};
  std::atomic<std::uint64_t> impactExecutionCount_{0U};
  std::atomic<std::uint64_t> contextGenerationCount_{0U};
  std::atomic<std::uint64_t> branchOperationCount_{0U};
  std::atomic<std::uint64_t> commitOperationCount_{0U};
  std::uint64_t temporalEventSequence_{0U};
  std::size_t temporalEventCount_{0U};
  std::size_t temporalRecentWindowCount_{0U};
  std::uint64_t lastMetaControlLoopMs_{0U};
  double lastCacheHitRate_{0.0};
  double lastRuntimeSignalScore_{0.0};
  std::string lastPatternPrediction_;
  std::string lastObservedCommand_;
  std::string lastTemporalCategory_;
  metacognition::MetaCognitiveSignal lastMetaSignal_{};
  policy_evolution::EvolutionAdjustment lastPolicyAdjustment_{};
  Options options_{};
};

DaemonRuntime::DaemonRuntime(std::filesystem::path projectRoot)
    : impl_(std::make_unique<Impl>(std::move(projectRoot))) {}

DaemonRuntime::~DaemonRuntime() {
  if (impl_) {
    impl_->stop();
  }
}

DaemonRuntime::DaemonRuntime(DaemonRuntime&& other) noexcept = default;
DaemonRuntime& DaemonRuntime::operator=(DaemonRuntime&& other) noexcept =
    default;

bool DaemonRuntime::start(const Options& options, std::string& error) {
  return impl_->start(options, error);
}

int DaemonRuntime::run(const Options& options, std::string& error) {
  return impl_->run(options, error);
}

void DaemonRuntime::requestStop() {
  impl_->requestStop();
}

void DaemonRuntime::stop() {
  impl_->stop();
}

bool DaemonRuntime::running() const noexcept {
  return impl_->running();
}

IpcResponse DaemonRuntime::dispatchRequest(const IpcRequest& request) {
  return impl_->dispatch(request);
}

nlohmann::json DaemonRuntime::statusPayload(const bool verboseStatus) const {
  return impl_->statusPayload(verboseStatus);
}

bool DaemonRuntime::contextDiff(nlohmann::json& payloadOut, std::string& error) {
  return impl_->contextDiff(payloadOut, error);
}

}  // namespace ultra::runtime
