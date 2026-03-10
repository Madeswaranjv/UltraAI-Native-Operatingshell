#include "IntentDecomposer.h"
#include "RuleBasedDecomposer.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace ultra::orchestration {

namespace {

void addTask(TaskGraph& graph,
             const std::string& taskId,
             const std::string& description,
             const float complexity) {
  SubTask task;
  task.taskId = taskId;
  task.description = description;
  task.expectedOutputs = {"result_payload"};
  task.estimatedComplexity = complexity;
  graph.addNode(task);
}

std::string taskIdForGoal(const ultra::runtime::intent::GoalType type) {
  using ultra::runtime::intent::GoalType;
  switch (type) {
    case GoalType::ModifySymbol:
      return "modify_symbol_body";
    case GoalType::RefactorModule:
      return "refactor_module";
    case GoalType::ReduceImpactRadius:
      return "reduce_impact_radius";
    case GoalType::ImproveCentrality:
      return "improve_centrality";
    case GoalType::MinimizeTokenUsage:
      return "minimize_token_usage";
    case GoalType::AddDependency:
      return "add_dependency";
    case GoalType::RemoveDependency:
      return "remove_dependency";
  }
  return "modify_symbol_body";
}

std::string descriptionForTask(const std::string& taskId,
                               const std::string& target) {
  if (taskId == "impact_prediction") {
    return "Predict deterministic impact for " + target;
  }
  if (taskId == "context_extraction") {
    return "Extract deterministic context for " + target;
  }
  if (taskId == "modify_symbol_body") {
    return "Plan contained symbol-body update for " + target;
  }
  if (taskId == "refactor_module") {
    return "Plan bounded module refactor for " + target;
  }
  if (taskId == "reduce_impact_radius") {
    return "Plan impact-radius reduction for " + target;
  }
  if (taskId == "improve_centrality") {
    return "Plan centrality improvement for " + target;
  }
  if (taskId == "minimize_token_usage") {
    return "Plan token-usage reduction for " + target;
  }
  if (taskId == "add_dependency") {
    return "Plan dependency introduction for " + target;
  }
  if (taskId == "remove_dependency") {
    return "Plan dependency removal for " + target;
  }
  if (taskId == "rename_symbol") {
    return "Plan deterministic rename for " + target;
  }
  if (taskId == "change_signature") {
    return "Plan bounded signature change for " + target;
  }
  if (taskId == "move_across_modules") {
    return "Plan deterministic cross-module move for " + target;
  }
  if (taskId == "update_public_api") {
    return "Plan explicit public API adjustment for " + target;
  }
  if (taskId == "update_references") {
    return "Plan deterministic reference updates for " + target;
  }
  if (taskId == "validate_plan") {
    return "Validate deterministic execution plan for " + target;
  }
  return "Plan task for " + target;
}

float complexityForTask(const std::string& taskId) {
  if (taskId == "impact_prediction" || taskId == "context_extraction") {
    return 1.0f;
  }
  if (taskId == "rename_symbol" || taskId == "change_signature" ||
      taskId == "update_references") {
    return 1.5f;
  }
  if (taskId == "move_across_modules" || taskId == "update_public_api") {
    return 2.0f;
  }
  if (taskId == "validate_plan") {
    return 0.75f;
  }
  return 1.25f;
}

std::vector<std::string> taskIdsForIntent(
    const ultra::runtime::intent::Intent& intent) {
  const bool symbolGoal =
      intent.goal.type == ultra::runtime::intent::GoalType::ModifySymbol ||
      intent.goal.type == ultra::runtime::intent::GoalType::ReduceImpactRadius ||
      intent.goal.type == ultra::runtime::intent::GoalType::ImproveCentrality ||
      intent.goal.type == ultra::runtime::intent::GoalType::MinimizeTokenUsage;
  std::vector<std::string> taskIds{
      "impact_prediction",
      "context_extraction",
      taskIdForGoal(intent.goal.type),
  };

  if (intent.options.allowRename && symbolGoal) {
    taskIds.push_back("rename_symbol");
  }
  if (intent.options.allowSignatureChange && symbolGoal) {
    taskIds.push_back("change_signature");
  }
  if (intent.options.allowCrossModuleMove) {
    taskIds.push_back("move_across_modules");
  }
  if (intent.options.allowPublicAPIChange) {
    taskIds.push_back("update_public_api");
  }
  if (intent.goal.type == ultra::runtime::intent::GoalType::RefactorModule ||
      intent.goal.type == ultra::runtime::intent::GoalType::AddDependency ||
      intent.goal.type == ultra::runtime::intent::GoalType::RemoveDependency ||
      intent.options.allowRename || intent.options.allowSignatureChange ||
      intent.options.allowCrossModuleMove) {
    taskIds.push_back("update_references");
  }

  taskIds.push_back("validate_plan");

  std::sort(taskIds.begin(), taskIds.end());
  taskIds.erase(std::unique(taskIds.begin(), taskIds.end()), taskIds.end());

  std::vector<std::string> ordered;
  ordered.reserve(taskIds.size());
  auto appendIfPresent = [&taskIds, &ordered](const std::string& taskId) {
    if (std::find(taskIds.begin(), taskIds.end(), taskId) != taskIds.end()) {
      ordered.push_back(taskId);
    }
  };

  appendIfPresent("impact_prediction");
  appendIfPresent("context_extraction");
  appendIfPresent(taskIdForGoal(intent.goal.type));
  appendIfPresent("rename_symbol");
  appendIfPresent("change_signature");
  appendIfPresent("move_across_modules");
  appendIfPresent("update_public_api");
  appendIfPresent("update_references");
  appendIfPresent("validate_plan");
  return ordered;
}

}  // namespace

IntentDecomposer::IntentDecomposer(std::unique_ptr<IDecomposer> strategy)
    : strategy_(std::move(strategy)) {
  if (!strategy_) {
    strategy_ = std::make_unique<RuleBasedDecomposer>();
  }
}

TaskGraph IntentDecomposer::decompose(const std::string& goal) const {
  if (strategy_) {
    return strategy_->decompose(goal);
  }
  return TaskGraph{};
}

TaskGraph IntentDecomposer::decompose(
    const ultra::runtime::intent::Intent& intent) const {
  TaskGraph graph;
  const std::string target =
      intent.goal.target.empty() ? ultra::runtime::intent::toString(intent.goal.type)
                                 : intent.goal.target;
  const std::vector<std::string> taskIds = taskIdsForIntent(intent);
  for (const std::string& taskId : taskIds) {
    addTask(graph, taskId, descriptionForTask(taskId, target),
            complexityForTask(taskId));
  }

  for (std::size_t index = 1U; index < taskIds.size(); ++index) {
    graph.addDependency(taskIds[index - 1U], taskIds[index]);
  }
  return graph;
}

}  // namespace ultra::orchestration
