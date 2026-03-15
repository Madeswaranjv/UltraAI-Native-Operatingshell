#include "AIContextGenerator.h"

#include "../core/ConfigManager.h"
#include "../graph/DependencyGraph.h"
#include "../scanner/FileInfo.h"

namespace ultra::ai {

GenerateResult AIContextGenerator::generate(
    const std::vector<ultra::scanner::FileInfo>& files,
    const ultra::graph::DependencyGraph& graph,
    const ultra::core::ConfigManager& config) {
  (void)files;
  (void)graph;
  (void)config;
  GenerateResult result;
  result.context = nlohmann::json::object();
  result.context["ast_mode"] = "disabled";
  result.context["reason"] =
      "AIContextGenerator has been disabled; use daemon semantic index APIs.";
  return result;
}

GenerateResult AIContextGenerator::generateWithAst(
    const std::vector<ultra::scanner::FileInfo>& files,
    const ultra::graph::DependencyGraph& graph,
    const ultra::core::ConfigManager& config) {
  return generate(files, graph, config);
}

std::vector<std::string> AIContextGenerator::getContextPathSet(
    const std::vector<ultra::scanner::FileInfo>& files,
    const ultra::graph::DependencyGraph& graph,
    const ultra::core::ConfigManager& config) {
  (void)files;
  (void)graph;
  (void)config;
  return {};
}

}  // namespace ultra::ai
