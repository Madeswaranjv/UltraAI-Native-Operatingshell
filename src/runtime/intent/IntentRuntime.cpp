#include "IntentRuntime.h"

#include "../../authority/IntentSimulator.h"
#include "../../authority/UltraAuthorityAPI.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace ultra::runtime::intent {

namespace {

[[nodiscard]] ::ultra::authority::AuthorityIntentRequest buildAuthorityIntentRequest(
    const std::string& input,
    const ContextFrame& frame) {
  ::ultra::authority::AuthorityIntentRequest request;
  request.goal = frame.goal.empty() ? input : frame.goal;
  request.target = frame.target;
  request.branchId = frame.branchId;
  request.tokenBudget = frame.tokenBudget;
  request.impactDepth = frame.impactDepth;
  request.maxFilesChanged = frame.maxFilesChanged;
  request.tolerance = frame.tolerance;
  request.allowPublicApiChange = frame.allowPublicApiChange;
  request.threshold = frame.riskThreshold;

  if (request.target.empty()) {
    request.target = request.goal;
  }
  return request;
}

[[nodiscard]] bool hasDependencyDiffType(
    const ::ultra::authority::SimulatedIntentResult& simulation,
    const ::ultra::diff::semantic::DiffType diffType) {
  return std::any_of(
      simulation.diffReport.dependencies.begin(),
      simulation.diffReport.dependencies.end(),
      [diffType](const ::ultra::diff::semantic::DependencyDiff& dependency) {
        return dependency.type == diffType;
      });
}

[[nodiscard]] GoalType classifyIntentGoal(
    const ::ultra::authority::AuthorityIntentRequest& request,
    const ::ultra::authority::SimulatedIntentResult& simulation) {
  if (hasDependencyDiffType(simulation, ::ultra::diff::semantic::DiffType::Removed)) {
    return GoalType::RemoveDependency;
  }

  if (hasDependencyDiffType(simulation, ::ultra::diff::semantic::DiffType::Added)) {
    return GoalType::AddDependency;
  }

  if (simulation.publicApiChanges > 0U || request.allowPublicApiChange) {
    return GoalType::RefactorModule;
  }

  if (simulation.impactDepth > std::max<std::size_t>(1U, request.impactDepth)) {
    return GoalType::ReduceImpactRadius;
  }

  if (request.tokenBudget > 0U && request.tokenBudget <= 512U) {
    return GoalType::MinimizeTokenUsage;
  }

  return GoalType::ModifySymbol;
}

[[nodiscard]] Intent buildStructuredIntent(
    const ::ultra::authority::AuthorityIntentRequest& request,
    const ::ultra::authority::SimulatedIntentResult& simulation,
    const std::size_t fallbackTokenBudget) {
  Intent intent;
  intent.goal.type = classifyIntentGoal(request, simulation);
  intent.goal.target = request.target.empty() ? request.goal : request.target;
  intent.constraints.maxImpactDepth = std::max<std::size_t>(1U, request.impactDepth);
  intent.constraints.maxFilesChanged =
      std::max<std::size_t>(1U, request.maxFilesChanged);
  intent.constraints.tokenBudget = request.tokenBudget;
  intent.constraints.branchScope = request.branchId;
  intent.constraints.determinismRequired = true;
  intent.risk = request.tolerance;
  intent.options.allowPublicAPIChange = request.allowPublicApiChange;

  return normalizeIntent(intent, fallbackTokenBudget);
}

}  // namespace

Intent IntentRuntime::process_input(const std::string& input) const {
  Intent intent;
  intent.goal.type = GoalType::ModifySymbol;
  intent.goal.target = input;
  return normalizeIntent(intent, 4096U);
}

::ultra::orchestration::TaskGraph IntentRuntime::decompose_intent(
    const Intent& intent) const {
  return decomposer_.decompose(intent);
}

IntentEvaluation IntentRuntime::evaluate_intent(
    const Intent& intent,
    const CognitiveState& state) const {
  return evaluator_.evaluateIntent(intent, state);
}

Intent IntentRuntime::resolve_structured_intent(const std::string& input,
                                                const ContextFrame& frame) const {
  const ::ultra::authority::AuthorityIntentRequest request =
      buildAuthorityIntentRequest(input, frame);
  if (request.goal.empty() && request.target.empty()) {
    throw std::runtime_error(
        "Intent stage did not provide a goal or target for runtime resolution.");
  }

  ::ultra::authority::IntentSimulator simulator(std::filesystem::current_path());
  const ::ultra::authority::SimulatedIntentResult simulation =
      simulator.simulate(request);

  const std::size_t fallbackTokenBudget =
      request.tokenBudget == 0U ? 4096U : request.tokenBudget;
  return buildStructuredIntent(request, simulation, fallbackTokenBudget);
}

}  // namespace ultra::runtime::intent

