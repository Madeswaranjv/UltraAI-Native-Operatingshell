#include "RiskEvaluator.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

namespace ultra::engine::impact {

namespace {

double clamp01(const double value) {
  return std::clamp(value, 0.0, 1.0);
}

double ratioScore(const std::size_t value, const std::size_t denominator) {
  if (denominator == 0U) {
    return 0.0;
  }
  return clamp01(static_cast<double>(value) /
                 static_cast<double>(denominator));
}

std::uint32_t toMicros(const double score) {
  return static_cast<std::uint32_t>(
      std::llround(clamp01(score) * 1000000.0));
}

}  // namespace

RiskAssessment RiskEvaluator::evaluate(
    const ImpactPlan& plan,
    const std::vector<ImpactedFile>& files,
    const std::vector<ImpactedSymbol>& symbols) const {
  RiskAssessment risk;
  if (files.empty() && symbols.empty()) {
    return risk;
  }

  for (const ImpactedFile& file : files) {
    risk.dependencyDepth = std::max(risk.dependencyDepth, file.depth);
  }

  double centralityAccumulator = 0.0;
  for (const ImpactedSymbol& symbol : symbols) {
    risk.dependencyDepth = std::max(risk.dependencyDepth, symbol.depth);
    centralityAccumulator += clamp01(symbol.centrality);
    if (symbol.publicApi) {
      ++risk.publicApiCount;
    }
  }
  risk.averageCentrality = symbols.empty()
                               ? 0.0
                               : centralityAccumulator /
                                     static_cast<double>(symbols.size());

  std::set<std::string> modules;
  for (const ImpactedFile& file : files) {
    modules.insert(moduleOf(file.path));
  }
  for (const ImpactedSymbol& symbol : symbols) {
    if (!symbol.definedIn.empty()) {
      modules.insert(moduleOf(symbol.definedIn));
    }
  }
  risk.affectedModuleCount = modules.size();
  risk.transitiveImpactSize = files.size() + symbols.size();

  const std::size_t depthBudget =
      std::max<std::size_t>(1U, std::max(plan.fileDepth, plan.symbolDepth));
  const std::size_t impactBudget =
      std::max<std::size_t>(1U,
                            std::min<std::size_t>(32U,
                                                  plan.maxFiles + plan.maxSymbols));

  const double depthScore = ratioScore(risk.dependencyDepth, depthBudget);
  const double centralityScore = clamp01(risk.averageCentrality);
  const double publicApiScore =
      symbols.empty()
          ? 0.0
          : clamp01(static_cast<double>(risk.publicApiCount) /
                    static_cast<double>(symbols.size()));
  const double moduleScore = ratioScore(risk.affectedModuleCount, 8U);
  const double transitiveScore =
      ratioScore(risk.transitiveImpactSize, impactBudget);

  const double weighted = (0.25 * depthScore) + (0.25 * centralityScore) +
                          (0.20 * publicApiScore) + (0.15 * moduleScore) +
                          (0.15 * transitiveScore);
  risk.scoreMicros = toMicros(weighted);
  risk.score = static_cast<double>(risk.scoreMicros) / 1000000.0;
  return risk;
}

std::string RiskEvaluator::moduleOf(const std::string& path) {
  if (path.empty()) {
    return {};
  }

  const std::filesystem::path normalized =
      std::filesystem::path(path).lexically_normal();
  auto it = normalized.begin();
  if (it == normalized.end()) {
    return path;
  }

  const std::string first = (*it).string();
  ++it;
  if (first == "src" && it != normalized.end()) {
    return (*it).string();
  }
  if (normalized.has_parent_path()) {
    const std::string parent = normalized.parent_path().generic_string();
    if (!parent.empty()) {
      return parent;
    }
  }
  return first;
}

}  // namespace ultra::engine::impact
