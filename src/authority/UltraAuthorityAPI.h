#pragma once

#include "../diff/BranchSemanticDiff.h"
#include "../runtime/CognitiveState.h"
#include "../runtime/governance/Policy.h"
#include "../runtime/intent/Intent.h"

#include <external/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ultra::authority {

struct AuthorityContextRequest {
  std::string query;
  std::string branchId;
  std::size_t tokenBudget{512U};
  std::size_t impactDepth{2U};
  runtime::RelevanceProfile relevanceProfile{};
};

struct AuthorityContextResult {
  bool success{false};
  std::string contextJson;
  std::size_t estimatedTokens{0U};
  std::uint64_t snapshotVersion{0U};
  std::string snapshotHash;
  std::string message;
};

struct AuthorityIntentRequest {
  std::string goal;
  std::string target;
  std::string branchId;
  std::size_t tokenBudget{4096U};
  std::size_t impactDepth{2U};
  std::size_t maxFilesChanged{8U};
  runtime::intent::RiskTolerance tolerance{
      runtime::intent::RiskTolerance::MEDIUM};
  bool allowPublicApiChange{false};
  double threshold{0.66};
};

struct AuthorityBranchRequest {
  std::string reason;
  std::string parentBranchId;
};

struct AuthorityCommitRequest {
  std::string sourceBranchId;
  std::string targetBranchId{"main"};
  double maxAllowedRisk{0.66};
  runtime::governance::Policy policy{};
};

struct AuthorityRiskReport {
  double score{0.0};
  std::size_t removedSymbols{0U};
  std::size_t signatureChanges{0U};
  std::size_t dependencyBreaks{0U};
  std::size_t publicApiChanges{0U};
  std::size_t impactDepth{0U};
  bool withinThreshold{false};
  diff::semantic::BranchDiffReport diffReport;
};

class UltraAuthorityAPI {
 public:
  explicit UltraAuthorityAPI(
      std::filesystem::path projectRoot = std::filesystem::current_path());

  [[nodiscard]] std::string createBranch(
      const AuthorityBranchRequest& request) const;
  [[nodiscard]] AuthorityRiskReport evaluateRisk(
      const AuthorityIntentRequest& request) const;
  [[nodiscard]] AuthorityContextResult getContextSlice(
      const AuthorityContextRequest& request) const;
  [[nodiscard]] bool commitWithPolicy(const AuthorityCommitRequest& request,
                                      std::string& error) const;
  [[nodiscard]] nlohmann::ordered_json getSavingsReport() const;

 private:
  std::filesystem::path projectRoot_;
};

}  // namespace ultra::authority
