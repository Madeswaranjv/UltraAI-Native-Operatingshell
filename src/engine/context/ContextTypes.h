#pragma once

#include "../../ai/RuntimeState.h"
#include "../query/SymbolQueryEngine.h"

#include <external/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ultra::engine::context {

enum class ContextKind : std::uint8_t { Symbol = 0U, File = 1U, Impact = 2U };

struct RankingWeights {
  double recencyWeight{0.35};
  double centralityWeight{0.25};
  double usageWeight{0.20};
  double impactWeight{0.20};
};

struct ContextRequest {
  ContextKind kind{ContextKind::Symbol};
  std::string target;
  std::size_t tokenBudget{512U};
  std::size_t impactDepth{2U};
  RankingWeights weights{};
};

struct ContextPlan {
  ContextRequest request;
  std::vector<std::string> rootSymbols;
  std::vector<std::string> rootFiles;
  std::vector<std::string> fileDependencies;
  std::vector<std::string> symbolDependencies;
  std::vector<std::string> impactFiles;
};

struct SymbolContextCandidate {
  std::uint64_t symbolId{0U};
  std::string name;
  std::string definedIn;
  std::vector<query::SymbolDefinition> definitions;
  std::vector<std::string> references;
  std::vector<std::string> dependencies;
  std::vector<std::string> impactFiles;
  double weight{0.0};
  double centrality{0.0};
  std::size_t distance{0U};
  bool isRoot{false};
};

struct FileContextCandidate {
  std::string path;
  std::vector<std::string> dependencies;
  std::vector<std::string> relevantSymbols;
  std::size_t distance{0U};
  bool isRoot{false};
};

struct RankedSymbolCandidate {
  SymbolContextCandidate candidate;
  std::int64_t scoreMicros{0};
  bool selected{true};
};

struct RankedFileCandidate {
  FileContextCandidate candidate;
  std::int64_t scoreMicros{0};
  bool selected{true};
};

struct ContextSlice {
  std::vector<std::uint64_t> includedNodes;
  nlohmann::ordered_json payload;
  std::string json;
  std::size_t estimatedTokens{0U};
  std::size_t rawEstimatedTokens{0U};
  std::size_t candidateSymbolCount{0U};
  std::size_t candidateFileCount{0U};
};

}  // namespace ultra::engine::context
