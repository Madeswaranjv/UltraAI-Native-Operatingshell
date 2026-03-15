#include "RiskEvaluator.h"

#include "UltraAuthorityAPI.h"

#include <algorithm>

namespace ultra::authority {

namespace {

double clamp01(const double value) {
  return std::clamp(value, 0.0, 1.0);
}

}  // namespace

AuthorityRiskReport RiskEvaluator::evaluate(
    const diff::semantic::BranchDiffReport& diffReport,
    const std::size_t impactDepth,
    const std::size_t publicApiChanges,
    const double threshold) const {
  AuthorityRiskReport report;
  report.diffReport = diffReport;
  report.impactDepth = impactDepth;
  report.publicApiChanges = publicApiChanges;

  report.removedSymbols = static_cast<std::size_t>(std::count_if(
      diffReport.symbols.begin(), diffReport.symbols.end(),
      [](const diff::semantic::SymbolDiff& symbol) {
        return symbol.type == diff::semantic::DiffType::Removed;
      }));
  report.signatureChanges = diffReport.signatures.size();
  report.dependencyBreaks = static_cast<std::size_t>(std::count_if(
      diffReport.dependencies.begin(), diffReport.dependencies.end(),
      [](const diff::semantic::DependencyDiff& dependency) {
        return dependency.type == diff::semantic::DiffType::Added ||
               dependency.type == diff::semantic::DiffType::Removed;
      }));

  const double removedComponent =
      clamp01(static_cast<double>(report.removedSymbols + report.publicApiChanges) /
              4.0);
  const double signatureComponent =
    clamp01(static_cast<double>(report.signatureChanges) / 100.0);
  const double dependencyComponent =
      clamp01((static_cast<double>(report.dependencyBreaks) +
               static_cast<double>(report.impactDepth)) /
              10.0);

  report.score = clamp01((removedComponent * 0.6) +
                         (signatureComponent * 0.3) +
                         (dependencyComponent * 0.1));
  report.withinThreshold = report.score <= clamp01(threshold);
  return report;
}

}  // namespace ultra::authority
