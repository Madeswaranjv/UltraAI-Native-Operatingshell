#include "ContextPruner.h"

#include "../query/QueryPlanner.h"

#include <algorithm>

namespace ultra::engine::context {

namespace {

bool definitionLess(const query::SymbolDefinition& left,
                    const query::SymbolDefinition& right) {
  if (left.filePath != right.filePath) {
    return left.filePath < right.filePath;
  }
  if (left.lineNumber != right.lineNumber) {
    return left.lineNumber < right.lineNumber;
  }
  return left.symbolId < right.symbolId;
}

bool definitionSame(const query::SymbolDefinition& left,
                    const query::SymbolDefinition& right) {
  return left.symbolId == right.symbolId;
}

}  // namespace

std::optional<ContextPruner::PruneDecision> ContextPruner::selectNextCandidate(
    const std::vector<RankedSymbolCandidate>& rankedSymbols,
    const std::vector<RankedFileCandidate>& rankedFiles) const {
  struct CandidateRef {
    PruneDecision decision;
    std::int64_t scoreMicros{0};
    std::string tieBreaker;
  };

  std::vector<CandidateRef> candidates;
  candidates.reserve(rankedSymbols.size() + rankedFiles.size());

  for (std::size_t index = 0U; index < rankedFiles.size(); ++index) {
    const RankedFileCandidate& ranked = rankedFiles[index];
    if (!ranked.selected || ranked.candidate.isRoot) {
      continue;
    }
    candidates.push_back(
        {{PruneDecision::Kind::File, index},
         ranked.scoreMicros,
         ranked.candidate.path});
  }

  for (std::size_t index = 0U; index < rankedSymbols.size(); ++index) {
    const RankedSymbolCandidate& ranked = rankedSymbols[index];
    if (!ranked.selected || ranked.candidate.isRoot) {
      continue;
    }
    candidates.push_back(
        {{PruneDecision::Kind::Symbol, index},
         ranked.scoreMicros,
         std::to_string(ranked.candidate.symbolId) + "|" + ranked.candidate.name});
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const CandidateRef& left, const CandidateRef& right) {
              if (left.scoreMicros != right.scoreMicros) {
                return left.scoreMicros < right.scoreMicros;
              }
              if (left.decision.kind != right.decision.kind) {
                return left.decision.kind == PruneDecision::Kind::File;
              }
              return left.tieBreaker < right.tieBreaker;
            });
  return candidates.front().decision;
}

void ContextPruner::collapseStrings(std::vector<std::string>& values) {
  query::QueryPlanner::sortAndDedupe(values);
}

void ContextPruner::collapseDefinitions(
    std::vector<query::SymbolDefinition>& definitions) {
  std::sort(definitions.begin(), definitions.end(), definitionLess);
  definitions.erase(
      std::unique(definitions.begin(), definitions.end(), definitionSame),
      definitions.end());
}

}  // namespace ultra::engine::context
