#include "ContextPlanner.h"

#include "../query/QueryPlanner.h"

#include <set>

namespace ultra::engine::context {

namespace {

std::vector<std::string> collectFileSymbols(const ai::RuntimeState& state,
                                            const std::string& filePath) {
  std::set<std::string> names;
  for (const auto& [name, node] : state.symbolIndex) {
    if (node.definedIn == filePath ||
        node.usedInFiles.find(filePath) != node.usedInFiles.end()) {
      names.insert(name);
    }
  }
  return std::vector<std::string>(names.begin(), names.end());
}

void sortAndDedupeStrings(std::vector<std::string>& values) {
  query::QueryPlanner::sortAndDedupe(values);
}

}  // namespace

ContextPlan ContextPlanner::planSymbolContext(
    const ContextRequest& request,
    const query::SymbolQueryEngine& queryEngine) const {
  ContextPlan plan;
  plan.request = request;
  if (!request.target.empty()) {
    plan.rootSymbols.push_back(request.target);
  }

  const auto definitions = queryEngine.findDefinition(request.target);
  for (const query::SymbolDefinition& definition : definitions) {
    if (!definition.filePath.empty()) {
      plan.rootFiles.push_back(definition.filePath);
      const auto deps = queryEngine.findFileDependencies(definition.filePath);
      plan.fileDependencies.insert(plan.fileDependencies.end(), deps.begin(),
                                   deps.end());
    }
  }

  plan.symbolDependencies = queryEngine.findSymbolDependencies(request.target);
  plan.impactFiles =
      queryEngine.findImpactRegion(request.target,
                                   std::max<std::size_t>(1U, request.impactDepth));

  sortAndDedupeStrings(plan.rootSymbols);
  sortAndDedupeStrings(plan.rootFiles);
  sortAndDedupeStrings(plan.fileDependencies);
  sortAndDedupeStrings(plan.symbolDependencies);
  sortAndDedupeStrings(plan.impactFiles);
  return plan;
}

ContextPlan ContextPlanner::planFileContext(
    const ContextRequest& request,
    const query::SymbolQueryEngine& queryEngine,
    const ai::RuntimeState& state,
    const std::string& resolvedFilePath) const {
  ContextPlan plan;
  plan.request = request;
  if (resolvedFilePath.empty()) {
    return plan;
  }

  plan.rootFiles.push_back(resolvedFilePath);
  plan.rootSymbols = collectFileSymbols(state, resolvedFilePath);
  plan.fileDependencies = queryEngine.findFileDependencies(resolvedFilePath);
  plan.impactFiles = plan.fileDependencies;

  sortAndDedupeStrings(plan.rootSymbols);
  sortAndDedupeStrings(plan.rootFiles);
  sortAndDedupeStrings(plan.fileDependencies);
  sortAndDedupeStrings(plan.impactFiles);
  return plan;
}

ContextPlan ContextPlanner::planImpactContext(
    const ContextRequest& request,
    const query::SymbolQueryEngine& queryEngine) const {
  ContextPlan plan;
  plan.request = request;
  if (!request.target.empty()) {
    plan.rootSymbols.push_back(request.target);
  }

  const auto definitions = queryEngine.findDefinition(request.target);
  for (const query::SymbolDefinition& definition : definitions) {
    if (!definition.filePath.empty()) {
      plan.rootFiles.push_back(definition.filePath);
      const auto deps = queryEngine.findFileDependencies(definition.filePath);
      plan.fileDependencies.insert(plan.fileDependencies.end(), deps.begin(),
                                   deps.end());
    }
  }

  plan.symbolDependencies = queryEngine.findSymbolDependencies(request.target);
  plan.impactFiles =
      queryEngine.findImpactRegion(request.target,
                                   std::max<std::size_t>(1U, request.impactDepth));

  sortAndDedupeStrings(plan.rootSymbols);
  sortAndDedupeStrings(plan.rootFiles);
  sortAndDedupeStrings(plan.fileDependencies);
  sortAndDedupeStrings(plan.symbolDependencies);
  sortAndDedupeStrings(plan.impactFiles);
  return plan;
}

}  // namespace ultra::engine::context
