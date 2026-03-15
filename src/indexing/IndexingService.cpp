#include "IndexingService.h"

#include "../graph/DependencyGraph.h"
#include "../incremental/IncrementalAnalyzer.h"
#include "../runtime/impact_analyzer.h"

#include <algorithm>
#include <exception>
#include <map>
#include <utility>

namespace ultra::indexing {

IndexingService::IndexingService(std::filesystem::path projectRoot,
                                 core::StateManager& stateManager)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()),
      stateManager_(stateManager),
      scanner_(projectRoot_),
      queryEngine_(projectRoot_) {}

void IndexingService::markRuntimeActive(ai::RuntimeState& state) {
  state.core.magic = ai::IntegrityManager::kCoreMagic;
  state.core.schemaVersion = ai::IntegrityManager::kSchemaVersion;
  state.core.indexVersion = ai::IntegrityManager::kIndexVersion;
  state.core.runtimeActive = 1U;
}

IndexSnapshot IndexingService::indexSnapshot() const {
  const ai::RuntimeState state = stateManager_.snapshotState();
  IndexSnapshot snapshot;
  snapshot.version = stateManager_.currentVersion();
  snapshot.filesIndexed = state.files.size();
  snapshot.symbolsIndexed = state.symbols.size();
  snapshot.dependenciesIndexed =
      state.deps.fileEdges.size() + state.deps.symbolEdges.size();
  return snapshot;
}

runtime::GraphSnapshot IndexingService::snapshot() const {
  return stateManager_.getSnapshot();
}

bool IndexingService::hasSemanticIndex() const {
  const ai::RuntimeState state = stateManager_.snapshotState();
  return state.core.runtimeActive == 1U &&
         state.core.schemaVersion == ai::IntegrityManager::kSchemaVersion &&
         state.core.indexVersion == ai::IntegrityManager::kIndexVersion;
}

bool IndexingService::loadOrBuild(IndexSnapshot& snapshotOut,
                                  std::string& error) {
  std::string persistedError;
  if (stateManager_.loadPersistedGraph(persistedError)) {
    ai::RuntimeState loaded = stateManager_.snapshotState();
    markRuntimeActive(loaded);
    stateManager_.replaceState(std::move(loaded));
    snapshotOut = indexSnapshot();
    error.clear();
    return true;
  }

  if (!buildIndex(projectRoot_, snapshotOut, error)) {
    if (!persistedError.empty() && error != persistedError) {
      error = "Failed to load persisted semantic index: " + persistedError +
              ". Rebuild failure: " + error;
    }
    return false;
  }

  return true;
}

bool IndexingService::buildIndex(const std::filesystem::path& projectRoot,
                                 IndexSnapshot& snapshotOut,
                                 std::string& error) {
  const std::filesystem::path normalizedRoot =
      std::filesystem::absolute(projectRoot).lexically_normal();
  if (normalizedRoot != projectRoot_) {
    error = "IndexingService root mismatch. Expected " + projectRoot_.string() +
            " but received " + normalizedRoot.string();
    return false;
  }

  engine::ScanOutput output;
  if (!scanner_.fullScanParallel(output, error)) {
    if (error.empty()) {
      error = "Tree-sitter semantic index build failed.";
    }
    return false;
  }

  ai::RuntimeState state;
  state.files = std::move(output.files);
  state.symbols = std::move(output.symbols);
  state.deps = std::move(output.deps);
  state.semanticSymbolDepsByFileId = std::move(output.semanticSymbolDepsByFileId);
  state.symbolIndex = std::move(output.symbolIndex);
  markRuntimeActive(state);

  stateManager_.replaceState(std::move(state));
  if (!stateManager_.persistGraphStore(error)) {
    return false;
  }

  snapshotOut = indexSnapshot();
  error.clear();
  return true;
}

bool IndexingService::rebuild(IndexSnapshot& snapshotOut, std::string& error) {
  return buildIndex(projectRoot_, snapshotOut, error);
}

void IndexingService::mergeScanOutput(engine::ScanOutput& aggregate,
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

graph::DependencyGraph IndexingService::dependencyGraphFromState(
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

std::vector<std::string> IndexingService::rebuildSetForChange(
    const ai::RuntimeState& state,
    const std::string& changedPath) const {
  if (changedPath.empty()) {
    return {};
  }
  const graph::DependencyGraph graph = dependencyGraphFromState(state);
  return incremental::IncrementalAnalyzer::computeRebuildSet({changedPath}, graph);
}

bool IndexingService::persistGraphDelta(const engine::ScanOutput& output,
                                        std::string& error) const {
  std::vector<std::uint32_t> touchedFileIds;
  touchedFileIds.reserve(output.changesForLog.size());
  for (const ai::ChangeLogRecord& change : output.changesForLog) {
    if (change.fileId != 0U) {
      touchedFileIds.push_back(change.fileId);
    }
  }

  std::sort(touchedFileIds.begin(), touchedFileIds.end());
  touchedFileIds.erase(
      std::unique(touchedFileIds.begin(), touchedFileIds.end()),
      touchedFileIds.end());

  if (touchedFileIds.empty()) {
    return stateManager_.persistGraphStore(error);
  }
  return stateManager_.persistGraphStoreIncremental(touchedFileIds, error);
}

bool IndexingService::applyIncrementalMutation(const runtime::DaemonEvent& event,
                                               engine::ScanOutput& output,
                                               std::string& error) {
  output = engine::ScanOutput{};
  std::string scanError;

  const bool applied = stateManager_.withWriteLock(
      [&](ai::RuntimeState& state, engine::WeightEngine& weights,
          memory::LruManager& lru) {
        const std::vector<std::string> rebuildSet =
            rebuildSetForChange(state, event.path);

        bool ok = false;
        switch (event.type) {
          case runtime::DaemonEventType::FileAdded:
            ok = scanner_.incrementalAdd(state, event.path, output, scanError);
            if (ok && !output.changeSet.added.empty()) {
              weights.incrementalAdd(event.path);
              lru.touch(event.path);
            }
            break;
          case runtime::DaemonEventType::FileModified:
            ok = scanner_.incrementalModify(state, event.path, output, scanError);
            if (ok && !output.changeSet.modified.empty()) {
              weights.incrementalModify(event.path);
              lru.touch(event.path);
            }
            break;
          case runtime::DaemonEventType::FileRemoved:
            ok = scanner_.incrementalRemove(state, event.path, output, scanError);
            if (ok && !output.changeSet.deleted.empty()) {
              weights.incrementalRemove(event.path);
              lru.erase(event.path);
            }
            break;
          case runtime::DaemonEventType::RebuildRequest:
          case runtime::DaemonEventType::ShutdownRequest:
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

        markRuntimeActive(state);
        return true;
      });

  if (!applied) {
    error = scanError.empty() ? "Incremental semantic mutation failed." : scanError;
    return false;
  }

  if (output.changeSet.empty()) {
    error.clear();
    return true;
  }

  return persistGraphDelta(output, error);
}

bool IndexingService::applyMutation(const runtime::DaemonEvent& event,
                                    engine::ScanOutput& output,
                                    std::string& error) {
  if (event.type == runtime::DaemonEventType::RebuildRequest) {
    IndexSnapshot snapshotOut;
    return rebuild(snapshotOut, error);
  }

  if (event.type == runtime::DaemonEventType::ShutdownRequest) {
    output = engine::ScanOutput{};
    error.clear();
    return true;
  }

  return applyIncrementalMutation(event, output, error);
}

SymbolQueryResult IndexingService::querySymbol(const std::string& symbolName) const {
  SymbolQueryResult result;
  if (symbolName.empty()) {
    result.payload = nlohmann::json{{"kind", "not_found"},
                                    {"target", symbolName}};
    return result;
  }

  const runtime::GraphSnapshot current = stateManager_.getSnapshot();
  result.payload = queryEngine_.querySymbol(current, symbolName);
  result.found = result.payload.value("kind", "") != "not_found";
  return result;
}

bool IndexingService::queryTarget(const runtime::GraphSnapshot& snapshot,
                                  const std::string& target,
                                  nlohmann::json& payloadOut,
                                  std::string& error) const {
  payloadOut = nlohmann::json::object();
  error.clear();
  try {
    // Semantic symbol lookups are the primary ai_query path.
    payloadOut = queryEngine_.querySymbol(snapshot, target);
    if (payloadOut.value("kind", "") != "not_found") {
      return true;
    }

    // File queries remain available for indexed-path targets.
    payloadOut = queryEngine_.queryTarget(snapshot, target);
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  } catch (...) {
    error = "Unknown semantic query failure.";
    return false;
  }
}

bool IndexingService::buildContext(const std::string& query,
                                   std::size_t tokenBudget,
                                   std::size_t impactDepth,
                                   ContextSliceResult& resultOut,
                                   std::string& error) const {
  resultOut = ContextSliceResult{};
  error.clear();

  const std::string trimmed = query;
  if (trimmed.empty()) {
    error = "Context query must be non-empty.";
    return false;
  }

  try {
    const std::size_t boundedBudget = std::max<std::size_t>(1U, tokenBudget);
    const std::size_t boundedDepth = std::max<std::size_t>(1U, impactDepth);

    const runtime::CognitiveState state =
        stateManager_.createCognitiveState(boundedBudget);

    runtime::Query contextQuery;
    contextQuery.kind = runtime::QueryKind::Auto;
    contextQuery.target = trimmed;
    contextQuery.impactDepth = boundedDepth;

    runtime::ContextExtractor extractor;
    const runtime::ContextSlice slice =
        extractor.getMinimalContext(state, contextQuery);

    nlohmann::json payload = nlohmann::json::parse(slice.json, nullptr, false);
    if (payload.is_discarded()) {
      error = "ContextBuilder returned invalid JSON payload.";
      return false;
    }

    resultOut.payload = std::move(payload);
    resultOut.estimatedTokens = slice.estimatedTokens;

    const nlohmann::json nodes =
        resultOut.payload.value("nodes", nlohmann::json::array());
    const nlohmann::json files =
        resultOut.payload.value("files", nlohmann::json::array());
    resultOut.found = (nodes.is_array() && !nodes.empty()) ||
                      (files.is_array() && !files.empty());
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  } catch (...) {
    error = "Unknown semantic context failure.";
    return false;
  }
}

bool IndexingService::analyzeImpact(const std::string& target,
                                    std::size_t impactDepth,
                                    ImpactReport& reportOut,
                                    std::string& error) const {
  reportOut = ImpactReport{};
  error.clear();

  if (target.empty()) {
    error = "Impact target must be non-empty.";
    return false;
  }

  try {
    const std::size_t boundedDepth = std::max<std::size_t>(1U, impactDepth);
    const runtime::GraphSnapshot current = stateManager_.getSnapshot();
    runtime::ImpactAnalyzer analyzer(current);

    const std::string resolvedFile =
        queryEngine_.resolveIndexedFilePath(current, target);
    if (!resolvedFile.empty()) {
      reportOut.payload = analyzer.analyzeFileImpact(resolvedFile, boundedDepth);
    } else {
      reportOut.payload = analyzer.analyzeSymbolImpact(target, boundedDepth);
    }
    reportOut.found = reportOut.payload.value("kind", "") != "not_found";
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  } catch (...) {
    error = "Unknown semantic impact analysis failure.";
    return false;
  }
}

bool IndexingService::readSource(const std::string& target,
                                 nlohmann::json& payloadOut,
                                 std::string& error) const {
  payloadOut = nlohmann::json::object();
  error.clear();

  const runtime::GraphSnapshot current = stateManager_.getSnapshot();
  const std::string indexedPath = queryEngine_.resolveIndexedFilePath(current, target);
  if (indexedPath.empty()) {
    error = "Source target is not indexed: " + target;
    return false;
  }

  return queryEngine_.readSourceByIndexedPath(indexedPath, payloadOut, error);
}

std::string IndexingService::resolveIndexedFilePath(
    const runtime::GraphSnapshot& snapshot,
    const std::string& target) const {
  return queryEngine_.resolveIndexedFilePath(snapshot, target);
}

}  // namespace ultra::indexing
