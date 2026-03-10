#include "UltraAuthorityAPI.h"

#include "CommitCoordinator.h"
#include "ContextSliceProvider.h"
#include "IntentSimulator.h"
#include "RiskEvaluator.h"

#include "../api/CognitiveKernelAPI.h"
#include "../intelligence/BranchLifecycle.h"
#include "../intelligence/BranchPersistence.h"
#include "../intelligence/BranchStore.h"
#include "../metrics/PerformanceMetrics.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ultra::authority {

namespace {

diff::semantic::DiffType mapDiffType(const api::DiffType type) {
  switch (type) {
    case api::DiffType::Added:
      return diff::semantic::DiffType::Added;
    case api::DiffType::Removed:
      return diff::semantic::DiffType::Removed;
    case api::DiffType::Renamed:
      return diff::semantic::DiffType::Renamed;
    case api::DiffType::Moved:
      return diff::semantic::DiffType::Moved;
    case api::DiffType::Modified:
      return diff::semantic::DiffType::Modified;
  }
  return diff::semantic::DiffType::Modified;
}

diff::semantic::SignatureChange mapSignatureChange(
    const api::SignatureChange change) {
  switch (change) {
    case api::SignatureChange::Signature:
      return diff::semantic::SignatureChange::Signature;
    case api::SignatureChange::Visibility:
      return diff::semantic::SignatureChange::Visibility;
    case api::SignatureChange::Relocation:
      return diff::semantic::SignatureChange::Relocation;
    case api::SignatureChange::Rename:
      return diff::semantic::SignatureChange::Rename;
  }
  return diff::semantic::SignatureChange::Signature;
}

diff::semantic::BranchDiffReport toSemanticReport(
    const api::BranchDiffReport& report) {
  diff::semantic::BranchDiffReport semanticReport;

  semanticReport.symbols.reserve(report.symbols.size());
  for (const api::SymbolDiff& symbol : report.symbols) {
    semanticReport.symbols.push_back({symbol.id, mapDiffType(symbol.type)});
  }

  semanticReport.signatures.reserve(report.signatures.size());
  for (const api::SignatureDiff& signature : report.signatures) {
    semanticReport.signatures.push_back(
        {signature.id, mapSignatureChange(signature.change)});
  }

  semanticReport.dependencies.reserve(report.dependencies.size());
  for (const api::DependencyDiff& dependency : report.dependencies) {
    semanticReport.dependencies.push_back(
        {dependency.from, dependency.to, mapDiffType(dependency.type)});
  }

  semanticReport.impactScore = report.impactScore;
  switch (report.overallRisk) {
    case api::RiskLevel::LOW:
      semanticReport.overallRisk = diff::semantic::RiskLevel::LOW;
      break;
    case api::RiskLevel::MEDIUM:
      semanticReport.overallRisk = diff::semantic::RiskLevel::MEDIUM;
      break;
    case api::RiskLevel::HIGH:
      semanticReport.overallRisk = diff::semantic::RiskLevel::HIGH;
      break;
  }
  return semanticReport;
}

}  // namespace

UltraAuthorityAPI::UltraAuthorityAPI(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()) {}

std::string UltraAuthorityAPI::createBranch(
    const AuthorityBranchRequest& request) const {
  if (request.reason.empty()) {
    throw std::invalid_argument("Branch creation requires a non-empty reason.");
  }

  intelligence::BranchStore store;
  intelligence::BranchPersistence persistence(projectRoot_ / ".ultra");
  (void)persistence.load(store);

  intelligence::BranchLifecycle lifecycle(store);
  const intelligence::Branch created =
      request.parentBranchId.empty()
          ? lifecycle.create(request.reason)
          : lifecycle.spawn(request.parentBranchId, request.reason);
  if (created.branchId.empty()) {
    throw std::runtime_error("Branch creation failed.");
  }
  if (!persistence.save(store)) {
    throw std::runtime_error("Branch creation failed: unable to persist state.");
  }
  return created.branchId;
}

AuthorityRiskReport UltraAuthorityAPI::evaluateRisk(
    const AuthorityIntentRequest& request) const {
  IntentSimulator simulator(projectRoot_);
  const SimulatedIntentResult simulation = simulator.simulate(request);

  RiskEvaluator evaluator;
  return evaluator.evaluate(simulation.diffReport, simulation.impactDepth,
                            simulation.publicApiChanges, request.threshold);
}

AuthorityContextResult UltraAuthorityAPI::getContextSlice(
    const AuthorityContextRequest& request) const {
  ContextSliceProvider provider(projectRoot_);
  return provider.getSlice(request);
}

bool UltraAuthorityAPI::commitWithPolicy(const AuthorityCommitRequest& request,
                                         std::string& error) const {
  if (request.sourceBranchId.empty() || request.targetBranchId.empty()) {
    error = "Commit requires non-empty source and target branch IDs.";
    return false;
  }
  try {
    const api::BranchDiffReport branchDiff = api::CognitiveKernelAPI::diffBranches(
        request.sourceBranchId, request.targetBranchId);
    const diff::semantic::BranchDiffReport semanticDiff =
        toSemanticReport(branchDiff);

    RiskEvaluator evaluator;
    const AuthorityRiskReport riskReport = evaluator.evaluate(
        semanticDiff,
        static_cast<std::size_t>(std::max(1, request.policy.maxImpactDepth)),
        semanticDiff.signatures.size(), request.maxAllowedRisk);

    CommitCoordinator coordinator(projectRoot_);
    return coordinator.commit(request, riskReport, error);
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  } catch (...) {
    error = "Commit failed due to an unknown authority-layer error.";
    return false;
  }
}

nlohmann::ordered_json UltraAuthorityAPI::getSavingsReport() const {
  const nlohmann::ordered_json metricsReport = metrics::PerformanceMetrics::report();
  const nlohmann::ordered_json tokenSection =
      metricsReport.value("token", nlohmann::ordered_json::object());

  nlohmann::ordered_json savings;
  savings["avg_token_savings_ratio"] =
      metricsReport.value("avg_token_savings_ratio", 0.0);
  savings["total_tokens_saved"] = tokenSection.value("total_tokens_saved", 0ULL);
  savings["avg_savings_percent"] = tokenSection.value("avg_savings_percent", 0.0);
  savings["estimated_llm_calls_avoided"] =
      tokenSection.value("estimated_llm_calls_avoided", 0ULL);
  savings["metrics"] = metricsReport;
  return savings;
}

}  // namespace ultra::authority
