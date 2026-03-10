#include "ContextSummaryGenerator.h"

#include "../context/TokenBudgetManager.h"

#include <algorithm>
#include <string>
#include <vector>

namespace ultra::engine::context_compression {

namespace {

bool eraseField(nlohmann::ordered_json& object, const char* key) {
  if (!object.is_object() || !object.contains(key)) {
    return false;
  }
  object.erase(key);
  return true;
}

bool clearArrayField(nlohmann::ordered_json& object, const char* key) {
  if (!object.is_object() || !object.contains(key) || !object[key].is_array() ||
      object[key].empty()) {
    return false;
  }
  object[key] = nlohmann::ordered_json::array();
  return true;
}

void sortAndDedupeStrings(std::vector<std::string>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::size_t countArrayItems(const nlohmann::ordered_json& object, const char* key) {
  if (!object.is_object() || !object.contains(key) || !object[key].is_array()) {
    return 0U;
  }
  return object[key].size();
}

std::size_t countModules(const nlohmann::ordered_json& hierarchy) {
  if (!hierarchy.is_object() || !hierarchy.contains("repository") ||
      !hierarchy["repository"].is_object()) {
    return 0U;
  }
  return countArrayItems(hierarchy["repository"], "modules");
}

std::string snapshotHashPrefix(const runtime::GraphSnapshot& snapshot) {
  try {
    const std::string hash = snapshot.deterministicHash();
    return hash.size() > 16U ? hash.substr(0, 16U) : hash;
  } catch (...) {
    return {};
  }
}

std::vector<std::string> collectRootDependencies(
    const context::ContextPlan& plan,
    const query::SymbolQueryEngine& queryEngine,
    const std::size_t maxDependencies) {
  std::vector<std::string> dependencies;
  for (const std::string& symbol : plan.rootSymbols) {
    const auto items = queryEngine.findSymbolDependencies(symbol);
    dependencies.insert(dependencies.end(), items.begin(), items.end());
  }
  sortAndDedupeStrings(dependencies);
  if (dependencies.size() > maxDependencies) {
    dependencies.resize(maxDependencies);
  }
  return dependencies;
}

bool compactSummary(nlohmann::ordered_json& summary,
                    const context::TokenBudgetManager& budgetManager) {
  auto currentTokens = [&summary, &budgetManager]() {
    return budgetManager.estimateTextTokens(summary.dump());
  };

  while (!budgetManager.fits(currentTokens())) {
    bool changed = false;

    if (!changed && summary.contains("snapshot") &&
        summary["snapshot"].is_object()) {
      changed = eraseField(summary["snapshot"], "hash");
    }
    if (!changed && summary.contains("root_dependencies")) {
      changed = eraseField(summary, "root_dependencies");
    }
    if (!changed && summary.contains("roots") && summary["roots"].is_object()) {
      changed = clearArrayField(summary["roots"], "files");
    }
    if (!changed && summary.contains("roots") && summary["roots"].is_object()) {
      changed = clearArrayField(summary["roots"], "symbols");
    }
    if (!changed && summary.contains("snapshot") &&
        summary["snapshot"].is_object()) {
      changed = eraseField(summary["snapshot"], "branch");
    }
    if (!changed && summary.contains("snapshot") &&
        summary["snapshot"].is_object() && summary["snapshot"].empty()) {
      changed = eraseField(summary, "snapshot");
    }
    if (!changed && summary.contains("roots")) {
      changed = eraseField(summary, "roots");
    }
    if (!changed) {
      break;
    }
  }

  return budgetManager.fits(currentTokens());
}

}  // namespace

nlohmann::ordered_json ContextSummaryGenerator::generateSummary(
    const runtime::GraphSnapshot& snapshot,
    const context::ContextPlan& plan,
    const query::SymbolQueryEngine& queryEngine,
    const nlohmann::ordered_json& hierarchy,
    const context::ContextSlice& slice,
    const std::size_t tokenBudget) const {
  nlohmann::ordered_json summary;

  nlohmann::ordered_json coverage;
  coverage["files"] = countArrayItems(slice.payload, "files");
  coverage["modules"] = countModules(hierarchy);
  coverage["symbols"] =
      slice.includedNodes.empty() ? countArrayItems(slice.payload, "nodes")
                                  : slice.includedNodes.size();
  summary["coverage"] = std::move(coverage);

  std::vector<std::string> rootFiles = plan.rootFiles;
  std::vector<std::string> rootSymbols = plan.rootSymbols;
  sortAndDedupeStrings(rootFiles);
  sortAndDedupeStrings(rootSymbols);
  if (!rootFiles.empty() || !rootSymbols.empty()) {
    nlohmann::ordered_json roots;
    roots["files"] = rootFiles;
    roots["symbols"] = rootSymbols;
    summary["roots"] = std::move(roots);
  }

  const std::vector<std::string> rootDependencies =
      collectRootDependencies(plan, queryEngine, 12U);
  if (!rootDependencies.empty()) {
    summary["root_dependencies"] = rootDependencies;
  }

  nlohmann::ordered_json snapshotInfo;
  if (!snapshot.branch.isNil()) {
    snapshotInfo["branch"] = snapshot.branch.toString();
  }
  const std::string hash = snapshotHashPrefix(snapshot);
  if (!hash.empty()) {
    snapshotInfo["hash"] = hash;
  }
  if (snapshot.version != 0U) {
    snapshotInfo["version"] = snapshot.version;
  }
  if (!snapshotInfo.empty()) {
    summary["snapshot"] = std::move(snapshotInfo);
  }

  const std::size_t maxSummaryTokens =
      std::max<std::size_t>(8U, std::min<std::size_t>((tokenBudget + 3U) / 4U, 160U));
  const context::TokenBudgetManager budgetManager(maxSummaryTokens);
  if (!compactSummary(summary, budgetManager)) {
    nlohmann::ordered_json fallback;
    fallback["coverage"] = summary["coverage"];
    return fallback;
  }

  return summary;
}

}  // namespace ultra::engine::context_compression
