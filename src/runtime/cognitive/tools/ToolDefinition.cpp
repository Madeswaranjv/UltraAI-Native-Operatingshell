#include "ToolDefinition.h"

#include <utility>
#include <vector>

namespace ultra::runtime::cognitive::tools {

bool ToolDefinition::is_valid() const noexcept {
  return !name.empty() && !description.empty() && !output_description.empty();
}

std::vector<ToolDefinition> defaultToolDefinitions() {
  std::vector<ToolDefinition> tools;
  tools.reserve(6U);

  tools.push_back(ToolDefinition{
      "query_symbol",
      "Resolve a symbol through Ultra indexing via 'ultra ai_query <target>'.",
      {"target"},
      "Structured symbol metadata, references, and impact region."});

  tools.push_back(ToolDefinition{
      "read_source",
      "Read source content via 'ultra ai_source <file>'.",
      {"file"},
      "Source file content for the requested indexed file."});

  tools.push_back(ToolDefinition{
      "impact_analysis",
      "Analyze change impact via 'ultra ai_impact <target>'.",
      {"target"},
      "Deterministic impact summary for the requested target."});

  tools.push_back(ToolDefinition{
      "get_context",
      "Fetch contextual graph summary via 'ultra ai_context <query>'.",
      {"query"},
      "Context package suitable for deterministic planning."});

  tools.push_back(ToolDefinition{
      "get_status",
      "Check daemon and index health via 'ultra ai_status --verbose'.",
      {},
      "Runtime daemon status and indexing diagnostics."});

  tools.push_back(ToolDefinition{
      "apply_patch",
      "Apply a unified diff via 'ultra apply_patch <path> <diff>'.",
      {},
      "Patch application result with verification and build status."});

  return tools;
}

}  // namespace ultra::runtime::cognitive::tools
