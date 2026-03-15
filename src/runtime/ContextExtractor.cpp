#include "ContextExtractor.h"

#include "../engine/context/ContextBuilder.h"
#include "../engine/query/QueryCache.h"
#include "../memory/HotSlice.h"
#include "../metacognition/MetaCognitiveOrchestrator.h"
#include "../memory/epoch/EpochGuard.h"
#include "../metrics/PerformanceMetrics.h"

#include <chrono>
#include <limits>
#include <stdexcept>

namespace ultra::runtime {

namespace {

engine::context::RankingWeights toRankingWeights(
    const RelevanceProfile& profile) {
  engine::context::RankingWeights weights;
  weights.recencyWeight = profile.recencyWeight;
  weights.centralityWeight = profile.centralityWeight;
  weights.usageWeight = profile.usageWeight;
  weights.impactWeight = profile.impactWeight;
  return weights;
}

std::string symbolNodeId(const SymbolID symbolId) {
  return "symbol:" + std::to_string(symbolId);
}

struct ContextRuntimeCache {
  engine::query::QueryCache cache{128U};
  memory::HotSlice hotSlice{memory::HotSlice::kMaxHotSliceEntries};
};

ContextRuntimeCache& contextCache() {
  static ContextRuntimeCache cache;
  return cache;
}

memory::StateNode makeSymbolNode(const std::uint64_t symbolId,
                                 const std::string& name,
                                 const std::string& definedIn,
                                 const std::uint64_t version) {
  memory::StateNode node;
  node.nodeId = symbolNodeId(symbolId);
  node.nodeType = memory::NodeType::Symbol;
  node.version = static_cast<std::uint32_t>(version & 0xFFFFFFFFULL);
  node.data["kind"] = "symbol";
  node.data["name"] = name;
  if (!definedIn.empty()) {
    node.data["defined_in"] = definedIn;
  }
  node.data["centrality"] = 0.0;
  return node;
}

memory::StateNode makeFileNode(const std::string& path,
                               const std::uint64_t version) {
  memory::StateNode node;
  node.nodeId = "file:" + path;
  node.nodeType = memory::NodeType::File;
  node.version = static_cast<std::uint32_t>(version & 0xFFFFFFFFULL);
  node.data["kind"] = "file";
  node.data["path"] = path;
  return node;
}

void updateHotSlice(memory::HotSlice& hotSlice,
                    const engine::context::ContextSlice& slice,
                    const std::string& target,
                    const std::uint64_t version) {
  hotSlice.bindToSnapshotVersion(version);
  const nlohmann::ordered_json payload = slice.payload;

  const nlohmann::ordered_json nodes =
      payload.value("nodes", nlohmann::ordered_json::array());
  for (const auto& node : nodes) {
    if (!node.is_object()) {
      continue;
    }
    const std::uint64_t symbolId = node.value("id", 0ULL);
    const std::string name = node.value("name", std::string{});
    if (symbolId == 0ULL || name.empty()) {
      continue;
    }
    const std::string definedIn = node.value("defined_in", std::string{});
    hotSlice.storeNode(makeSymbolNode(symbolId, name, definedIn, version), version);
  }

  const nlohmann::ordered_json files =
      payload.value("files", nlohmann::ordered_json::array());
  for (const auto& file : files) {
    if (!file.is_object()) {
      continue;
    }
    const std::string path = file.value("path", std::string{});
    if (path.empty()) {
      continue;
    }
    hotSlice.storeNode(makeFileNode(path, version), version);
  }

  const nlohmann::ordered_json impact =
      payload.value("impact_region", nlohmann::ordered_json::array());
  for (const auto& item : impact) {
    if (!item.is_string()) {
      continue;
    }
    const std::string path = item.get<std::string>();
    if (path.empty()) {
      continue;
    }
    hotSlice.storeNode(makeFileNode(path, version), version);
  }

  if (!target.empty()) {
    memory::StateNode node;
    node.nodeId = "symbol_name:" + target;
    node.nodeType = memory::NodeType::Symbol;
    node.version = static_cast<std::uint32_t>(version & 0xFFFFFFFFULL);
    node.data["kind"] = "symbol";
    node.data["name"] = target;
    hotSlice.storeNode(node, version);
  }
}

ContextSlice toRuntimeSlice(const engine::context::ContextSlice& builtSlice) {
  ContextSlice slice;
  slice.includedNodes = builtSlice.includedNodes;
  slice.json = builtSlice.json;
  slice.estimatedTokens = builtSlice.estimatedTokens;
  return slice;
}

}  // namespace

ContextSlice ContextExtractor::getMinimalContext(const CognitiveState& state,
                                                 const Query& query) const {
  memory::epoch::EpochGuard guard(memory::epoch::EpochManager::instance());
  const auto startedAt = std::chrono::steady_clock::now();
  if (state.budget == 0U) {
    throw std::runtime_error("Token budget must be greater than zero.");
  }
  if (!state.snapshot.runtimeState) {
    throw std::runtime_error(
        "Graph snapshot is missing semantic runtime-state data.");
  }

  ContextRuntimeCache& cache = contextCache();
  const std::string cacheKey = query.target;
  engine::context::ContextSlice builtSlice;

  if (!cacheKey.empty() &&
      cache.cache.get(cacheKey, state.snapshot.version, builtSlice)) {
    updateHotSlice(cache.hotSlice, builtSlice, cacheKey, state.snapshot.version);
    const auto _ = metacognition::MetaCognitiveOrchestrator::instance().recordQuery(
        cacheKey,
        state.snapshot.version,
        state.budget,
        cache.cache.capacity(),
        cache.hotSlice.maxSize());
    (void)_;
    return toRuntimeSlice(builtSlice);
  }

  engine::context::ContextBuilder builder(
      *state.snapshot.runtimeState,
      state.snapshot.graphStore,
      state.snapshot.version,
      {.enableCompression = true, .graphSnapshot = &state.snapshot});

  const engine::context::RankingWeights weights = toRankingWeights(state.weights);
  switch (query.kind) {
    case QueryKind::Symbol:
      builtSlice = builder.buildSymbolContext(query.target, state.budget, weights,
                                              query.impactDepth);
      break;
    case QueryKind::File:
      builtSlice =
          builder.buildFileContext(query.target, state.budget, weights, query.impactDepth);
      break;
    case QueryKind::Impact:
      if (builder.hasSymbol(query.target)) {
        builtSlice = builder.buildImpactContext(query.target, state.budget, weights,
                                               query.impactDepth);
      } else {
        builtSlice =
            builder.buildFileContext(query.target, state.budget, weights, query.impactDepth);
      }
      break;
    case QueryKind::Auto:
    default:
      if (builder.hasSymbol(query.target)) {
        builtSlice = builder.buildSymbolContext(query.target, state.budget, weights,
                                                query.impactDepth);
      } else {
        builtSlice =
            builder.buildFileContext(query.target, state.budget, weights, query.impactDepth);
      }
      break;
  }

  if (!cacheKey.empty()) {
    cache.cache.put(cacheKey, state.snapshot.version, builtSlice);
  }

  updateHotSlice(cache.hotSlice, builtSlice, cacheKey, state.snapshot.version);
  const auto _ = metacognition::MetaCognitiveOrchestrator::instance().recordQuery(
      cacheKey,
      state.snapshot.version,
      state.budget,
      cache.cache.capacity(),
      cache.hotSlice.maxSize());
  (void)_;

  if (metrics::PerformanceMetrics::isEnabled()) {
    std::size_t hotSliceHits = 0U;
    for (const SymbolID symbolId : builtSlice.includedNodes) {
      if (cache.hotSlice.containsNode(symbolNodeId(symbolId),
                                      state.snapshot.version)) {
        ++hotSliceHits;
      }
    }
    metrics::ContextMetrics contextMetric;
    contextMetric.durationMicros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt)
            .count());
    contextMetric.candidateSymbolCount = builtSlice.candidateSymbolCount;
    contextMetric.selectedSymbolCount = builtSlice.includedNodes.size();
    contextMetric.jsonSizeBytes = builtSlice.json.size();
    contextMetric.estimatedTokens = builtSlice.estimatedTokens;
    contextMetric.rawEstimatedTokens =
        builtSlice.rawEstimatedTokens == 0U ? builtSlice.estimatedTokens
                                            : builtSlice.rawEstimatedTokens;
    if (builtSlice.candidateSymbolCount == 0U) {
      contextMetric.truncationRatio = 0.0;
    } else {
      contextMetric.truncationRatio =
          static_cast<double>(builtSlice.candidateSymbolCount -
                              builtSlice.includedNodes.size()) /
          static_cast<double>(builtSlice.candidateSymbolCount);
    }
    contextMetric.hotSliceHits = hotSliceHits;
    contextMetric.hotSliceLookups = builtSlice.includedNodes.size();
    metrics::PerformanceMetrics::recordContextMetric(contextMetric);
    metrics::PerformanceMetrics::recordTokenSavings(
        contextMetric.rawEstimatedTokens, builtSlice.estimatedTokens);
    metrics::PerformanceMetrics::recordHotSliceLookup(contextMetric.hotSliceHits,
                                                      contextMetric.hotSliceLookups);
    metrics::PerformanceMetrics::recordContextReuse(
        contextMetric.hotSliceHits, contextMetric.selectedSymbolCount);
  }

  return toRuntimeSlice(builtSlice);
}

}  // namespace ultra::runtime


