#include "ContextRanker.h"

#include <algorithm>
#include <cmath>

namespace ultra::engine::context {

namespace {

double clamp01(const double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, 0.0, 1.0);
}

std::int64_t toScoreMicros(const double value) {
  return static_cast<std::int64_t>(std::llround(value * 1000000.0));
}

double distanceScore(const std::size_t distance) {
  return 1.0 / static_cast<double>(distance + 1U);
}

double ratioScore(const std::size_t numerator, const std::size_t bias) {
  if (numerator == 0U) {
    return 0.0;
  }
  return clamp01(static_cast<double>(numerator) /
                 static_cast<double>(numerator + bias));
}

}  // namespace

std::vector<RankedSymbolCandidate> ContextRanker::rankSymbols(
    const std::vector<SymbolContextCandidate>& candidates,
    const ContextRequest& request) const {
  std::vector<RankedSymbolCandidate> ranked;
  ranked.reserve(candidates.size());

  for (const SymbolContextCandidate& candidate : candidates) {
    const double recencyScore = clamp01(candidate.weight / 4.0);
    const double centralityScore = clamp01(candidate.centrality);
    const double usageScore = ratioScore(candidate.references.size(), 4U);
    const double impactScore = ratioScore(candidate.impactFiles.size(), 3U);
    const double proximityScore = distanceScore(candidate.distance);
    const double rootBoost = candidate.isRoot ? 0.40 : 0.0;

    const double score =
        rootBoost + (request.weights.recencyWeight * recencyScore) +
        (request.weights.centralityWeight * centralityScore) +
        (request.weights.usageWeight * usageScore) +
        (request.weights.impactWeight * impactScore) + (0.15 * proximityScore);

    RankedSymbolCandidate rankedCandidate;
    rankedCandidate.candidate = candidate;
    rankedCandidate.scoreMicros = toScoreMicros(score);
    ranked.push_back(std::move(rankedCandidate));
  }

  std::sort(ranked.begin(), ranked.end(),
            [](const RankedSymbolCandidate& left,
               const RankedSymbolCandidate& right) {
              if (left.scoreMicros != right.scoreMicros) {
                return left.scoreMicros > right.scoreMicros;
              }
              if (left.candidate.symbolId != right.candidate.symbolId) {
                return left.candidate.symbolId < right.candidate.symbolId;
              }
              return left.candidate.name < right.candidate.name;
            });
  return ranked;
}

std::vector<RankedFileCandidate> ContextRanker::rankFiles(
    const std::vector<FileContextCandidate>& candidates,
    const ContextRequest& request) const {
  std::vector<RankedFileCandidate> ranked;
  ranked.reserve(candidates.size());

  for (const FileContextCandidate& candidate : candidates) {
    const double proximityScore = distanceScore(candidate.distance);
    const double dependencyScore =
        ratioScore(candidate.dependencies.size(), 3U);
    const double symbolCoverageScore =
        ratioScore(candidate.relevantSymbols.size(), 2U);
    const double rootBoost = candidate.isRoot ? 0.35 : 0.0;

    const double score =
        rootBoost + (request.weights.impactWeight * proximityScore) +
        (request.weights.centralityWeight * dependencyScore) +
        (request.weights.usageWeight * symbolCoverageScore);

    RankedFileCandidate rankedCandidate;
    rankedCandidate.candidate = candidate;
    rankedCandidate.scoreMicros = toScoreMicros(score);
    ranked.push_back(std::move(rankedCandidate));
  }

  std::sort(ranked.begin(), ranked.end(),
            [](const RankedFileCandidate& left,
               const RankedFileCandidate& right) {
              if (left.scoreMicros != right.scoreMicros) {
                return left.scoreMicros > right.scoreMicros;
              }
              return left.candidate.path < right.candidate.path;
            });
  return ranked;
}

}  // namespace ultra::engine::context
