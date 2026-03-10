#pragma once

#include "../context/ContextTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ultra::engine::impact {

enum class ImpactTargetKind : std::uint8_t { Symbol = 0U, File = 1U };

enum class TraversalDirection : std::uint8_t { Forward = 0U, Reverse = 1U };

struct ImpactedFile {
  std::string path;
  std::size_t depth{0U};
  bool isRoot{false};
  std::vector<std::string> affectedSymbols;
};

struct ImpactedSymbol {
  std::uint64_t symbolId{0U};
  std::string name;
  std::string definedIn;
  std::uint32_t lineNumber{0U};
  std::size_t depth{0U};
  bool isRoot{false};
  bool publicApi{false};
  double centrality{0.0};
};

struct RiskAssessment {
  double score{0.0};
  std::uint32_t scoreMicros{0U};
  std::size_t dependencyDepth{0U};
  double averageCentrality{0.0};
  std::size_t publicApiCount{0U};
  std::size_t affectedModuleCount{0U};
  std::size_t transitiveImpactSize{0U};
};

struct ImpactPlan {
  ImpactTargetKind targetKind{ImpactTargetKind::Symbol};
  std::string target;
  std::vector<std::string> rootFiles;
  std::vector<std::string> rootSymbols;
  std::vector<std::string> fileTraversalSeeds;
  std::vector<std::string> symbolTraversalSeeds;
  TraversalDirection fileDirection{TraversalDirection::Reverse};
  TraversalDirection symbolDirection{TraversalDirection::Reverse};
  std::size_t fileDepth{0U};
  std::size_t symbolDepth{0U};
  std::size_t maxFiles{64U};
  std::size_t maxSymbols{128U};
  std::vector<std::string> boundaryFiles;
  std::vector<std::string> boundarySymbols;
  context::ContextSlice context;
};

struct ImpactPrediction {
  ImpactTargetKind targetKind{ImpactTargetKind::Symbol};
  std::string target;
  std::vector<ImpactedFile> files;
  std::vector<ImpactedSymbol> symbols;
  std::vector<std::string> affectedFiles;
  std::vector<std::string> affectedSymbols;
  std::vector<std::string> impactRegion;
  RiskAssessment risk;
  context::ContextSlice context;
};

struct SimulationResult {
  ImpactPrediction prediction;
  std::vector<std::string> potentialBreakages;
  bool runtimeStateMutated{false};
};

}  // namespace ultra::engine::impact
