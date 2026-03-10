#include "GovernanceEngine.h"

#include "../../memory/CognitiveMemoryManager.h"
#include "../../metacognition/MetaCognitiveOrchestrator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace ultra::runtime::governance {

namespace {

constexpr double kRequiredDeterminismThreshold = 0.80;

bool isPublicApiAction(const intent::Action& action) {
  return action.publicApiSurface ||
         action.kind == intent::ActionKind::UpdatePublicAPI;
}

bool isCrossModuleMoveAction(const intent::Action& action) {
  return action.kind == intent::ActionKind::MoveAcrossModules;
}

int clampToInt(const std::size_t value) {
  if (value > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(value);
}

double clamp01(const double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::clamp(value, 0.0, 1.0);
}

int scalePolicyLimit(const int baseLimit,
                     const double multiplier) {
  const double scaled =
      static_cast<double>(std::max(1, baseLimit)) * multiplier;
  if (!std::isfinite(scaled)) {
    return std::max(1, baseLimit);
  }
  const double clamped = std::clamp(
      scaled, 1.0,
      static_cast<double>(std::numeric_limits<int>::max()));
  return static_cast<int>(std::lround(clamped));
}

void applyMetaPolicyModes(const metacognition::MetaCognitiveSignal& signal,
                          Policy& policy) {
  if (signal.enterConservativeMode) {
    policy.requireDeterminism = true;
    policy.allowPublicAPIChange = false;
    policy.allowCrossModuleMove = false;
    return;
  }

  if (signal.enterExploratoryMode) {
    policy.allowPublicAPIChange = true;
    policy.allowCrossModuleMove = true;
  }
}

void applyEvolutionAdjustments(
    const policy_evolution::EvolutionAdjustment& adjustment,
    Policy& policy,
    double& determinismThreshold) {
  const double riskMultiplier =
      std::clamp(1.0 - adjustment.riskBiasShift, 0.8, 1.2);
  const double explorationMultiplier =
      std::clamp(1.0 + adjustment.explorationBiasShift, 0.8, 1.2);
  const double combinedMultiplier =
      std::clamp(riskMultiplier * explorationMultiplier, 0.8, 1.2);

  policy.maxImpactDepth = scalePolicyLimit(policy.maxImpactDepth,
                                           combinedMultiplier);
  policy.maxFilesChanged = scalePolicyLimit(policy.maxFilesChanged,
                                            combinedMultiplier);
  policy.maxTokenBudget = scalePolicyLimit(policy.maxTokenBudget,
                                           combinedMultiplier);

  const double determinismMultiplier =
      std::clamp(1.0 + adjustment.determinismBiasShift, 0.8, 1.2);
  determinismThreshold = std::clamp(
      determinismThreshold * determinismMultiplier, 0.50, 0.95);
}

}  // namespace

GovernanceEngine::GovernanceEngine(memory::CognitiveMemoryManager* memoryManager)
    : memoryManager_(memoryManager) {}

void GovernanceEngine::bindMemoryManager(
    memory::CognitiveMemoryManager* memoryManager) {
  std::lock_guard<std::mutex> lock(memoryMutex_);
  memoryManager_ = memoryManager;
}

GovernanceReport GovernanceEngine::evaluate(const intent::Strategy& strategy,
                                            const Policy& policy,
                                            const CognitiveState& state) const {
  GovernanceReport report;
  report.risk = strategy.risk;
  report.impact = strategy.impact;
  report.tokenCost = strategy.tokenCost;
  report.determinism = strategy.determinism;

  memory::CognitiveMemoryManager* memoryManager = nullptr;
  {
    std::lock_guard<std::mutex> lock(memoryMutex_);
    memoryManager = memoryManager_;
  }

  Policy effectivePolicy =
      normalizePolicy(policy, clampToInt(state.budget));
  double requiredDeterminismThreshold = kRequiredDeterminismThreshold;
  if (memoryManager != nullptr) {
    metacognition::MetaCognitiveOrchestrator meta;
    const metacognition::MetaCognitiveSignal signal =
        meta.evaluate(memoryManager->strategic, memoryManager->episodic);
    const policy_evolution::EvolutionAdjustment adjustment =
        adaptiveEngine_.update(signal);
    applyMetaPolicyModes(signal, effectivePolicy);
    applyEvolutionAdjustments(adjustment, effectivePolicy,
                              requiredDeterminismThreshold);
  }

  if (!state.snapshot.graph) {
    report.violations.push_back("Snapshot graph is unavailable.");
  }

  if (intent::riskRank(report.risk.classification) >
      intent::riskRank(effectivePolicy.maxRisk)) {
    report.violations.push_back(
        "Risk classification exceeds policy.maxRisk.");
  }

  if (clampToInt(report.impact.dependencyDepth) > effectivePolicy.maxImpactDepth) {
    report.violations.push_back(
        "Impact depth exceeds policy.maxImpactDepth.");
  }

  if (clampToInt(report.impact.estimatedFiles) > effectivePolicy.maxFilesChanged) {
    report.violations.push_back(
        "Estimated files changed exceeds policy.maxFilesChanged.");
  }

  if (clampToInt(report.tokenCost.estimatedTokens) > effectivePolicy.maxTokenBudget) {
    report.violations.push_back(
        "Token estimate exceeds policy.maxTokenBudget.");
  }

  if (!effectivePolicy.allowPublicAPIChange) {
    const bool touchesPublicApi = std::any_of(
        strategy.proposedActions.begin(), strategy.proposedActions.end(),
        [](const intent::Action& action) { return isPublicApiAction(action); });
    if (touchesPublicApi) {
      report.violations.push_back(
          "Strategy requires public API change but policy forbids it.");
    }
  }

  if (!effectivePolicy.allowCrossModuleMove) {
    const bool movesAcrossModules = std::any_of(
        strategy.proposedActions.begin(), strategy.proposedActions.end(),
        [](const intent::Action& action) { return isCrossModuleMoveAction(action); });
    if (movesAcrossModules) {
      report.violations.push_back(
          "Strategy requires cross-module move but policy forbids it.");
    }
  }

  report.determinism.value = clamp01(report.determinism.value);
  if (effectivePolicy.requireDeterminism &&
      report.determinism.value < requiredDeterminismThreshold) {
    report.violations.push_back(
        "Determinism score is below required threshold.");
  }

  report.approved = report.violations.empty();
  report.reason = report.approved ? "Approved by governance policy."
                                  : "Rejected by governance policy.";

  if (memoryManager != nullptr) {
    memoryManager->recordRiskEvaluation("governance_risk:" + strategy.name,
                                        state.snapshot, report.risk.value,
                                        report.approved ? 0.20 : 0.90,
                                        report.determinism.value,
                                        "governance_risk_classified");
    memoryManager->recordMergeOutcome("governance:" + strategy.name, state.snapshot,
                                      report.approved, report.reason);
    memory::StrategicOutcome outcome;
    outcome.version = state.snapshot.version;
    outcome.category = "governance";
    outcome.subject = strategy.name;
    outcome.success = report.approved;
    outcome.rolledBack = !report.approved;
    outcome.predictedRisk = clamp01(report.risk.value);
    outcome.observedRisk = report.approved ? 0.1 : 0.9;
    outcome.estimatedTokens = report.tokenCost.estimatedTokens;
    outcome.compressedTokens = report.tokenCost.estimatedTokens;
    outcome.predictedConfidence = report.determinism.value;
    outcome.observedConfidence = report.approved ? 0.85 : 0.15;
    memoryManager->strategic.recordOutcome(outcome);
    memoryManager->applyStrategicAdjustments();
  }

  return report;
}

}  // namespace ultra::runtime::governance
