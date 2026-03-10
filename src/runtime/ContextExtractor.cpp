#include "ContextExtractor.h"

#include "../engine/context/ContextBuilder.h"
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

  engine::context::ContextBuilder builder(
      *state.snapshot.runtimeState,
      state.snapshot.graphStore,
      state.snapshot.version,
      {.enableCompression = true, .graphSnapshot = &state.snapshot});

  const engine::context::RankingWeights weights = toRankingWeights(state.weights);
  engine::context::ContextSlice builtSlice;
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

  if (metrics::PerformanceMetrics::isEnabled()) {
    std::size_t hotSliceHits = 0U;
    for (const SymbolID symbolId : builtSlice.includedNodes) {
      if (state.workingSet.containsNode(symbolNodeId(symbolId),
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

  ContextSlice slice;
  slice.includedNodes = std::move(builtSlice.includedNodes);
  slice.json = std::move(builtSlice.json);
  slice.estimatedTokens = builtSlice.estimatedTokens;
  return slice;
}

}  // namespace ultra::runtime
