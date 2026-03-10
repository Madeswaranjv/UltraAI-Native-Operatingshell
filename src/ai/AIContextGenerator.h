#pragma once

#include "external/json.hpp"
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::core {
class ConfigManager;
}
namespace ultra::graph {
class DependencyGraph;
}
namespace ultra::scanner {
struct FileInfo;
}

namespace ultra::ai {

struct ContextStats {
  std::size_t totalFiles{0};
  std::size_t includedFiles{0};
  std::size_t filteredFiles{0};
  std::size_t totalBytes{0};
  std::size_t includedBytes{0};
};

struct GenerateResult {
  nlohmann::json context;
  ContextStats stats;
  /** Path keys of files included in context (for snapshot). */
  std::vector<std::string> includedPathKeys;
};

class AIContextGenerator {
 public:
  static GenerateResult generate(
      const std::vector<ultra::scanner::FileInfo>& files,
      const ultra::graph::DependencyGraph& graph,
      const ultra::core::ConfigManager& config);

  /** Like generate() but uses AST extraction (libclang or regex fallback)
   *  and includes line numbers in the output. */
  static GenerateResult generateWithAst(
      const std::vector<ultra::scanner::FileInfo>& files,
      const ultra::graph::DependencyGraph& graph,
      const ultra::core::ConfigManager& config);

  /** Path keys of files that would be included in context (same filter as generate). */
  static std::vector<std::string> getContextPathSet(
      const std::vector<ultra::scanner::FileInfo>& files,
      const ultra::graph::DependencyGraph& graph,
      const ultra::core::ConfigManager& config);
};

}  // namespace ultra::ai
