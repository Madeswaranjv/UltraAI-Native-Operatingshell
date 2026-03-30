$ErrorActionPreference = 'Stop'

function Replace-OrFail {
  param(
    [string]$Path,
    [string]$Old,
    [string]$New
  )

  $content = Get-Content -Raw -Path $Path
  if (-not $content.Contains($Old)) {
    throw "Pattern not found in $Path"
  }

  $updated = $content.Replace($Old, $New)
  Set-Content -Path $Path -Value $updated
}

Replace-OrFail 'src/runtime/cognitive/ultra_loop.h' @'
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ultra::runtime::cognitive {
'@ @'
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ultra::memory {
class CognitiveMemoryManager;
}

namespace ultra::runtime::cognitive {
'@

Replace-OrFail 'src/runtime/cognitive/ultra_loop.h' @'
  bool replanRequested{false};
  bool reanchorRequested{false};
  const ::ultra::runtime::CognitiveState* cognitiveState{nullptr};

  std::string intentGoal;
'@ @'
  bool replanRequested{false};
  bool reanchorRequested{false};
  const ::ultra::runtime::CognitiveState* cognitiveState{nullptr};
  const ::ultra::memory::CognitiveMemoryManager* memoryManager{nullptr};

  std::string intentGoal;
'@

Replace-OrFail 'src/runtime/cognitive/micro_planner.h' @'
#include <string>
#include <vector>

namespace ultra::runtime::cognitive {
'@ @'
#include <string>
#include <vector>

namespace ultra::memory {
class CognitiveMemoryManager;
}

namespace ultra::runtime::cognitive {
'@

Replace-OrFail 'src/runtime/cognitive/micro_planner.h' @'
struct MicroPlanInput {
  std::string intentId;
  std::string strategyId;
  std::string planId;
  std::vector<TaskPayload> taskPayloads;
};
'@ @'
struct MicroPlanInput {
  std::string intentId;
  std::string strategyId;
  std::string planId;
  std::string goalType;
  const ::ultra::memory::CognitiveMemoryManager* memoryManager{nullptr};
  std::vector<TaskPayload> taskPayloads;
};
'@

Replace-OrFail 'src/runtime/cognitive/failure_recovery.h' @'
#include <optional>
#include <string>

namespace ultra::runtime::cognitive {
'@ @'
#include <optional>
#include <string>

namespace ultra::memory {
class CognitiveMemoryManager;
}

namespace ultra::runtime::cognitive {
'@

Replace-OrFail 'src/runtime/cognitive/failure_recovery.h' @'
  std::size_t retry_count{0U};
  std::size_t retry_limit{0U};
  std::optional<DependencyState> dependency_state{};
};
'@ @'
  std::size_t retry_count{0U};
  std::size_t retry_limit{0U};
  std::optional<DependencyState> dependency_state{};
  const ::ultra::memory::CognitiveMemoryManager* memoryManager{nullptr};
};
'@

Replace-OrFail 'src/runtime/cognitive/strategy_planner.h' @'
#include <optional>

namespace ultra::runtime::cognitive {
'@ @'
#include <optional>

namespace ultra::memory {
class CognitiveMemoryManager;
}

namespace ultra::runtime::cognitive {
'@

Replace-OrFail 'src/runtime/cognitive/strategy_planner.h' @'
class StrategyPlanner {
 public:
  [[nodiscard]] intent::Strategy generate(const intent::Intent& intentValue) const;
  void setModel(model::IModel* model) noexcept;

 private:
  [[nodiscard]] intent::Strategy generateDeterministic(
      const intent::Intent& intentValue) const;
  [[nodiscard]] std::optional<intent::Strategy> tryGenerateWithModel(
      const intent::Intent& intentValue) const;
'@ @'
class StrategyPlanner {
 public:
  [[nodiscard]] intent::Strategy generate(
      const intent::Intent& intentValue,
      const ::ultra::memory::CognitiveMemoryManager* memoryManager = nullptr) const;
  void setModel(model::IModel* model) noexcept;

 private:
  [[nodiscard]] intent::Strategy generateDeterministic(
      const intent::Intent& intentValue,
      const ::ultra::memory::CognitiveMemoryManager* memoryManager) const;
  [[nodiscard]] std::optional<intent::Strategy> tryGenerateWithModel(
      const intent::Intent& intentValue,
      const ::ultra::memory::CognitiveMemoryManager* memoryManager) const;
'@

Replace-OrFail 'src/runtime/intent/IntentRuntime.h' @'
#include <cstddef>
#include <string>

namespace ultra::runtime::intent {
'@ @'
#include <cstddef>
#include <string>

namespace ultra::memory {
class CognitiveMemoryManager;
}

namespace ultra::runtime::intent {
'@

Replace-OrFail 'src/runtime/intent/IntentRuntime.h' @'
class IntentRuntime {
 public:
  [[nodiscard]] Intent process_input(const std::string& input) const;
'@ @'
class IntentRuntime {
 public:
  explicit IntentRuntime(
      const ::ultra::memory::CognitiveMemoryManager* memoryManager = nullptr) noexcept;

  [[nodiscard]] Intent process_input(const std::string& input) const;
'@

Replace-OrFail 'src/runtime/intent/IntentRuntime.h' @'
 private:
  [[maybe_unused]] ::ultra::orchestration::IntentDecomposer decomposer_{};
  [[maybe_unused]] IntentEvaluator evaluator_{};
};
'@ @'
 private:
  const ::ultra::memory::CognitiveMemoryManager* memoryManager_{nullptr};
  [[maybe_unused]] ::ultra::orchestration::IntentDecomposer decomposer_{};
  [[maybe_unused]] IntentEvaluator evaluator_{};
};
'@

Replace-OrFail 'src/runtime/intent/IntentRuntime.cpp' @'
#include "IntentRuntime.h"

#include "../../authority/IntentSimulator.h"
#include "../../authority/UltraAuthorityAPI.h"
'@ @'
#include "IntentRuntime.h"

#include "../../authority/IntentSimulator.h"
#include "../../authority/UltraAuthorityAPI.h"
#include "../../memory/CognitiveMemoryManager.h"
'@

Replace-OrFail 'src/runtime/intent/IntentRuntime.cpp' @'
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
'@ @'
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

[[nodiscard]] std::size_t countFailureMatches(
    const std::vector<::ultra::memory::EpisodicMemoryMatch>& matches) {
  return static_cast<std::size_t>(std::count_if(
      matches.begin(), matches.end(), [](const auto& match) {
        return !match.success || match.rolledBack || match.type == "execution_failure" ||
               match.type == "rollback" || match.type == "merge_rejected";
      }));
}

[[nodiscard]] std::size_t countSuccessMatches(
    const std::vector<::ultra::memory::EpisodicMemoryMatch>& matches) {
  return static_cast<std::size_t>(std::count_if(
      matches.begin(), matches.end(), [](const auto& match) {
        return match.success && !match.rolledBack;
      }));
}

[[nodiscard]] std::size_t countStrategicFailures(
    const std::vector<::ultra::memory::StrategicMemoryMatch>& matches) {
  return static_cast<std::size_t>(std::count_if(
      matches.begin(), matches.end(), [](const auto& match) {
        return !match.success || match.rolledBack;
      }));
}

[[nodiscard]] std::size_t countStrategicSuccesses(
    const std::vector<::ultra::memory::StrategicMemoryMatch>& matches) {
  return static_cast<std::size_t>(std::count_if(
      matches.begin(), matches.end(), [](const auto& match) {
        return match.success && !match.rolledBack;
      }));
}

void applyMemoryIntentAdjustments(
    Intent& intent,
    const ::ultra::memory::CognitiveMemoryManager* memoryManager) {
  if (memoryManager == nullptr) {
    return;
  }

  const ::ultra::memory::MemoryQuery query = memoryManager->query(4U);
  const std::string goalKey = toString(intent.goal.type);
  const std::string signature = goalKey + ":" + intent.goal.target;
  const auto episodicMatches = query.getEpisodic(signature);
  const auto strategicMatches = query.getStrategic(goalKey);
  auto failureMatches = query.getFailures(signature);
  if (failureMatches.empty()) {
    failureMatches = query.getFailures(goalKey);
  }

  const std::size_t failureCount = countFailureMatches(episodicMatches) +
                                   countFailureMatches(failureMatches) +
                                   countStrategicFailures(strategicMatches);
  const std::size_t successCount = countSuccessMatches(episodicMatches) +
                                   countStrategicSuccesses(strategicMatches);

  if (failureCount <= successCount) {
    return;
  }

  intent.constraints.maxImpactDepth = std::max<std::size_t>(
      1U, std::min<std::size_t>(intent.constraints.maxImpactDepth, 2U));
  intent.constraints.maxFilesChanged = std::max<std::size_t>(
      1U,
      std::min<std::size_t>(intent.constraints.maxFilesChanged,
                            std::max<std::size_t>(1U, intent.constraints.maxFilesChanged / 2U)));

  if (failureMatches.size() >= 2U) {
    intent.options.allowPublicAPIChange = false;
  }

  if (intent.risk == RiskTolerance::HIGH) {
    intent.risk = RiskTolerance::MEDIUM;
  } else if (intent.risk == RiskTolerance::MEDIUM && failureMatches.size() >= 2U) {
    intent.risk = RiskTolerance::LOW;
  }
}
'@

Replace-OrFail 'src/runtime/intent/IntentRuntime.cpp' @'
}  // namespace

Intent IntentRuntime::process_input(const std::string& input) const {
'@ @'
}  // namespace

IntentRuntime::IntentRuntime(
    const ::ultra::memory::CognitiveMemoryManager* memoryManager) noexcept
    : memoryManager_(memoryManager) {}

Intent IntentRuntime::process_input(const std::string& input) const {
'@

Replace-OrFail 'src/runtime/intent/IntentRuntime.cpp' @'
  const std::size_t fallbackTokenBudget =
      request.tokenBudget == 0U ? 4096U : request.tokenBudget;
  return buildStructuredIntent(request, simulation, fallbackTokenBudget);
}
'@ @'
  const std::size_t fallbackTokenBudget =
      request.tokenBudget == 0U ? 4096U : request.tokenBudget;
  Intent intent = buildStructuredIntent(request, simulation, fallbackTokenBudget);
  applyMemoryIntentAdjustments(intent, memoryManager_);
  return normalizeIntent(intent, fallbackTokenBudget);
}
'@
