#pragma once

#include "Intent.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ultra::runtime::intent {

enum class ActionKind : std::uint8_t {
  ModifySymbolBody = 0,
  RefactorModule = 1,
  ReduceImpactRadius = 2,
  ImproveCentrality = 3,
  MinimizeTokenUsage = 4,
  AddDependency = 5,
  RemoveDependency = 6,
  RenameSymbol = 7,
  ChangeSignature = 8,
  UpdatePublicAPI = 9,
  MoveAcrossModules = 10
};

struct Action {
  ActionKind kind{ActionKind::ModifySymbolBody};
  std::string target;
  std::string details;
  std::size_t estimatedFilesChanged{1U};
  std::size_t estimatedDependencyDepth{1U};
  bool publicApiSurface{false};
};

struct RiskScore {
  double value{0.0};
  RiskTolerance classification{RiskTolerance::LOW};
  RiskTolerance tolerance{RiskTolerance::MEDIUM};
};

struct ImpactScore {
  double radius{0.0};
  std::size_t estimatedFiles{0U};
  std::size_t dependencyDepth{0U};
  double centrality{0.0};
  std::size_t maxFilesConstraint{0U};
  std::size_t maxDepthConstraint{0U};
};

struct DeterminismScore {
  double value{1.0};
  bool required{true};
};

struct TokenCostEstimate {
  std::size_t estimatedTokens{0U};
  std::size_t budget{0U};
  bool withinBudget{true};
};

struct Strategy {
  std::string name;
  std::vector<Action> proposedActions;
  RiskScore risk;
  ImpactScore impact;
  DeterminismScore determinism;
  TokenCostEstimate tokenCost;
};

[[nodiscard]] std::string toString(ActionKind kind);

}  // namespace ultra::runtime::intent

