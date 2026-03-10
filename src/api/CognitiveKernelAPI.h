#pragma once

#include <external/json.hpp>
#include "../runtime/CognitiveState.h"
#include "../runtime/governance/GovernanceReport.h"
#include "../runtime/governance/Policy.h"
#include "../runtime/intent/Intent.h"
#include "../runtime/intent/PlanScore.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::ai {
class AiRuntimeManager;
}  // namespace ultra::ai

namespace ultra::api {

using NodeID = std::string;
using Intent = runtime::intent::Intent;
using Policy = runtime::governance::Policy;

enum class QueryKind {
  Auto = 0,
  Symbol = 1,
  File = 2,
  Impact = 3
};

struct Query {
  QueryKind kind{QueryKind::Auto};
  std::string target;
  std::size_t impactDepth{2U};
};

struct ImpactRegion {
  NodeID id;
  std::vector<std::string> direct;
  std::vector<std::string> transitive;
  double impactScore{0.0};
};

enum class DiffType {
  Added = 0,
  Removed = 1,
  Renamed = 2,
  Moved = 3,
  Modified = 4
};

struct SymbolDiff {
  NodeID id;
  DiffType type{DiffType::Modified};
};

enum class SignatureChange {
  Signature = 0,
  Visibility = 1,
  Relocation = 2,
  Rename = 3
};

struct SignatureDiff {
  NodeID id;
  SignatureChange change{SignatureChange::Signature};
};

struct DependencyDiff {
  NodeID from;
  NodeID to;
  DiffType type{DiffType::Modified};
};

enum class RiskLevel {
  LOW = 0,
  MEDIUM = 1,
  HIGH = 2
};

struct BranchDiffReport {
  std::vector<SymbolDiff> symbols;
  std::vector<SignatureDiff> signatures;
  std::vector<DependencyDiff> dependencies;
  RiskLevel overallRisk{RiskLevel::LOW};
  double impactScore{0.0};
};

struct HotSliceStats {
  std::size_t currentSize{0U};
  std::size_t capacity{0U};
};

class CognitiveKernelAPI {
 public:
  static std::string getMinimalContext(const runtime::CognitiveState& state,
                                       const Query& query);

  static ImpactRegion getImpactRegion(const runtime::CognitiveState& state,
                                      NodeID id);

  static BranchDiffReport diffBranches(const std::string& branchA,
                                       const std::string& branchB);

  static std::vector<runtime::intent::PlanScore> evaluateIntent(
      const runtime::CognitiveState& state,
      Intent intent,
      runtime::TokenBudget budget);

  static std::vector<runtime::governance::GovernedStrategyResult>
  evaluateAndGovernIntent(const runtime::CognitiveState& state,
                          Intent intent,
                          Policy policy,
                          runtime::TokenBudget budget);

  static std::string compressContext(const runtime::CognitiveState& state);

  static double estimateTokenSavings();

  static HotSliceStats getHotSliceStats();

 private:
  friend class ultra::ai::AiRuntimeManager;

  static nlohmann::json queryTarget(const runtime::CognitiveState& state,
                                    const std::string& target,
                                    const std::filesystem::path& projectRoot);

  static nlohmann::json queryImpact(const runtime::CognitiveState& state,
                                    const std::string& target,
                                    const std::filesystem::path& projectRoot);

  static bool readSource(const runtime::CognitiveState& state,
                         const std::string& fileTarget,
                         const std::filesystem::path& projectRoot,
                         nlohmann::json& payloadOut,
                         std::string& error);
};

const char* toString(DiffType type);
const char* toString(SignatureChange change);
const char* toString(RiskLevel level);

}  // namespace ultra::api
