#pragma once

#include "ContextPlanner.h"
#include "ContextPruner.h"
#include "ContextRanker.h"
#include "TokenBudgetManager.h"

#include "../../core/graph_store/GraphStore.h"
#include "../query/SymbolQueryEngine.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ultra::runtime {
struct GraphSnapshot;
}

namespace ultra::engine::context {

struct ContextBuilderOptions {
  bool enableCompression{false};
  const runtime::GraphSnapshot* graphSnapshot{nullptr};
};

class ContextBuilder {
 public:
  explicit ContextBuilder(ai::RuntimeState state,
                          core::graph_store::GraphStore* graphStore = nullptr,
                          std::uint64_t stateVersion = 1U,
                          ContextBuilderOptions options = {});

  [[nodiscard]] bool hasSymbol(const std::string& symbolName) const;
  [[nodiscard]] std::string resolveFilePath(const std::string& filePath) const;

  [[nodiscard]] ContextSlice buildSymbolContext(
      const std::string& symbolName,
      std::size_t tokenBudget = 512U,
      const RankingWeights& weights = {},
      std::size_t impactDepth = 2U) const;
  [[nodiscard]] ContextSlice buildFileContext(
      const std::string& filePath,
      std::size_t tokenBudget = 512U,
      const RankingWeights& weights = {},
      std::size_t impactDepth = 2U) const;
  [[nodiscard]] ContextSlice buildImpactContext(
      const std::string& symbolName,
      std::size_t tokenBudget = 512U,
      const RankingWeights& weights = {},
      std::size_t impactDepth = 2U) const;

 private:
  ContextSlice buildContext(const ContextPlan& plan) const;
  ContextSlice maybeCompressSlice(const ContextPlan& plan,
                                  const TokenBudgetManager& budgetManager,
                                  ContextSlice slice) const;

  std::vector<SymbolContextCandidate> buildSymbolCandidates(
      const ContextPlan& plan) const;
  std::vector<FileContextCandidate> buildFileCandidates(
      const ContextPlan& plan) const;
  ContextSlice buildSlice(const ContextPlan& plan,
                          const std::vector<RankedSymbolCandidate>& rankedSymbols,
                          const std::vector<RankedFileCandidate>& rankedFiles,
                          const TokenBudgetManager& budgetManager,
                          std::size_t rawEstimatedTokens) const;

  std::uint64_t primarySymbolId(const std::string& symbolName) const;
  std::vector<std::string> relevantSymbolsForFile(
      const std::string& filePath) const;

  ai::RuntimeState state_;
  core::graph_store::GraphStore* graphStore_{nullptr};
  query::SymbolQueryEngine queryEngine_;
  ContextPlanner planner_;
  ContextRanker ranker_;
  ContextPruner pruner_;
  ContextBuilderOptions options_;
  std::map<std::string, std::string> canonicalFilePaths_;
  std::map<std::string, std::vector<std::string>> definedSymbolsByFile_;
  std::map<std::string, std::vector<std::string>> referencedSymbolsByFile_;
  std::map<std::string, std::vector<ai::SymbolRecord>> symbolsByName_;
};

}  // namespace ultra::engine::context
