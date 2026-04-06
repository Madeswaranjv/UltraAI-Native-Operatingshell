#include "ToolDefinition.h"

#include <utility>
#include <vector>

namespace ultra::runtime::cognitive::tools {

bool ToolDefinition::is_valid() const noexcept {
  return !name.empty() && !description.empty() && !output_description.empty();
}

std::vector<ToolDefinition> defaultToolDefinitions() {
  std::vector<ToolDefinition> tools;
  tools.reserve(13U);

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
      "read_file",
      "Read a workspace file directly through ToolRouter.",
      {"path"},
      "Local file content and size metadata."});

  tools.push_back(ToolDefinition{
      "write_file",
      "Write a workspace file through ToolRouter.",
      {"path", "content"},
      "Deterministic write result for the requested file."});

  tools.push_back(ToolDefinition{
      "append_file",
      "Append content to a workspace file through ToolRouter.",
      {"path", "content"},
      "Deterministic append result for the requested file."});

  tools.push_back(ToolDefinition{
      "delete_file",
      "Delete a workspace file through ToolRouter.",
      {"path"},
      "Deterministic delete result for the requested file."});

  tools.push_back(ToolDefinition{
      "list_dir",
      "List workspace directory contents through ToolRouter.",
      {},
      "Directory entries with type and size metadata."});

  tools.push_back(ToolDefinition{
      "search_files",
      "Search workspace files through ToolRouter.",
      {"pattern"},
      "Matching files, line numbers, and content snippets."});

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

  tools.push_back(ToolDefinition{
      "run_command",
      "Run a workspace command through ToolRouter.",
      {"command"},
      "Command stdout, stderr, and exit status."});

  return tools;
}

}  // namespace ultra::runtime::cognitive::tools
