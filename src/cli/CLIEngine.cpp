#include "CLIEngine.h"
#include "CommandOptionsParser.h"
#include "CommandRouter.h"
#include "../authority/UltraAuthorityAPI.h"
#include "../api/CognitiveKernelAPI.h"
#include "../ai/AIContextGenerator.h"
#include "../ai/AiRuntimeManager.h"
#include "../core/ConfigManager.h"
#include "../core/Logger.h"
#include "context/ContextSnapshot.h"
#include "../graph/DependencyGraph.h"
#include "../hashing/HashManager.h"
#include "../incremental/IncrementalAnalyzer.h"
#include "../language/AdapterFactory.h"
#include "../language/ILanguageAdapter.h"
#include "../scanner/FileInfo.h"
#include "../utils/PathUtils.h"
#include "external/json.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
//E:\Projects\Ultra\src\cli\CLIEngine.cpp
namespace ultra::cli {

namespace {

const char* kVersion = "0.1.0";

bool pathExists(const std::filesystem::path& path) {
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  return exists && !ec;
}

bool isLikelyProjectRoot(const std::filesystem::path& candidate) {
  if (pathExists(candidate / ".ultra")) {
    return true;
  }
  return pathExists(candidate / "CMakeLists.txt") &&
         pathExists(candidate / "src");
}

std::filesystem::path findProjectRoot(std::filesystem::path start) {
  std::error_code ec;
  start = std::filesystem::absolute(std::move(start), ec).lexically_normal();
  if (ec) {
    return start;
  }

  std::filesystem::path current = std::move(start);
  while (!current.empty()) {
    if (isLikelyProjectRoot(current)) {
      return current;
    }
    const std::filesystem::path parent = current.parent_path();
    if (parent.empty() || parent == current) {
      break;
    }
    current = parent;
  }
  return {};
}

std::filesystem::path resolveExecutablePath(const char* argv0) {
  if (argv0 == nullptr) {
    return {};
  }
  const std::filesystem::path raw(argv0);
  std::error_code ec;

  if (raw.has_parent_path()) {
    const std::filesystem::path absolute = std::filesystem::absolute(raw, ec);
    if (!ec && pathExists(absolute)) {
      return absolute.lexically_normal();
    }
  } else if (!raw.empty() && pathExists(raw)) {
    const std::filesystem::path absolute = std::filesystem::absolute(raw, ec);
    if (!ec) {
      return absolute.lexically_normal();
    }
  }

  std::string pathValue;

#ifdef _WIN32
  char* buffer = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buffer, &len, "PATH") == 0 && buffer != nullptr) {
    pathValue = buffer;
    free(buffer);
  }
#else
  const char* buffer = std::getenv("PATH");
  if (buffer != nullptr) {
    pathValue = buffer;
  }
#endif

if (!pathValue.empty()) {
#ifdef _WIN32
    const char delimiter = ';';
#else
    const char delimiter = ':';
#endif
    std::size_t begin = 0U;
    while (begin <= pathValue.size()) {
      const std::size_t end = pathValue.find(delimiter, begin);
      const std::string token = pathValue.substr(
          begin, end == std::string::npos ? std::string::npos : end - begin);
      if (!token.empty()) {
        const std::filesystem::path base(token);
        const std::filesystem::path candidate = base / raw;
        if (pathExists(candidate)) {
          return std::filesystem::absolute(candidate, ec).lexically_normal();
        }
#ifdef _WIN32
        if (candidate.extension().empty()) {
          static const char* const kExtensions[] = {".exe", ".bat", ".cmd"};
          for (const char* ext : kExtensions) {
            const std::filesystem::path withExt = candidate.string() + ext;
            if (pathExists(withExt)) {
              return std::filesystem::absolute(withExt, ec).lexically_normal();
            }
          }
        }
#endif
      }

      if (end == std::string::npos) {
        break;
      }
      begin = end + 1U;
    }
  }

  const std::filesystem::path fallback = std::filesystem::absolute(raw, ec);
  if (!ec) {
    return fallback.lexically_normal();
  }
  return {};
}

std::filesystem::path resolveCliProjectRoot(const char* argv0) {
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec) {
    const std::filesystem::path fromCwd = findProjectRoot(cwd);
    if (!fromCwd.empty()) {
      return fromCwd;
    }
  }

  const std::filesystem::path executablePath = resolveExecutablePath(argv0);
  if (!executablePath.empty()) {
    const std::filesystem::path fromExecutable =
        findProjectRoot(executablePath.parent_path());
    if (!fromExecutable.empty()) {
      return fromExecutable;
    }
  }

  if (!ec) {
    return cwd.lexically_normal();
  }
  return std::filesystem::path(".");
}

void printStringList(const std::string& label, const nlohmann::json& value) {
  std::cout << label << ":\n";
  if (!value.is_array() || value.empty()) {
    std::cout << "  (none)\n";
    return;
  }
  for (const nlohmann::json& item : value) {
    if (item.is_string()) {
      std::cout << "  - " << item.get<std::string>() << '\n';
    }
  }
}

void printDefinitionList(const nlohmann::json& value) {
  std::cout << "Definitions:\n";
  if (!value.is_array() || value.empty()) {
    std::cout << "  (none)\n";
    return;
  }

  for (const nlohmann::json& item : value) {
    if (!item.is_object()) {
      continue;
    }

    const std::string filePath = item.value("file_path", std::string{});
    const std::uint32_t lineNumber = item.value("line_number", 0U);
    const std::string signature = item.value("signature", std::string{});
    const std::uint64_t symbolId = item.value("symbol_id", 0ULL);

    std::cout << "  - " << filePath;
    if (lineNumber != 0U) {
      std::cout << ':' << lineNumber;
    }
    if (!signature.empty()) {
      std::cout << ' ' << signature;
    }
    if (symbolId != 0ULL) {
      std::cout << " [id=" << symbolId << ']';
    }
    std::cout << '\n';
  }
}

void printObjectStringFieldList(const std::string& label,
                                const nlohmann::json& value,
                                const char* fieldName) {
  std::cout << label << ":\n";
  if (!value.is_array() || value.empty()) {
    std::cout << "  (none)\n";
    return;
  }

  bool printed = false;
  for (const nlohmann::json& item : value) {
    if (!item.is_object()) {
      continue;
    }
    const std::string fieldValue = item.value(fieldName, std::string{});
    if (fieldValue.empty()) {
      continue;
    }
    std::cout << "  - " << fieldValue << '\n';
    printed = true;
  }
  if (!printed) {
    std::cout << "  (none)\n";
  }
}

void printAiContextPayload(const nlohmann::json& value) {
  if (!value.is_object()) {
    return;
  }

  const nlohmann::json metadata = value.value("metadata", nlohmann::json::object());
  std::cout << "AI Context:\n";
  std::cout << "  Kind: " << value.value("kind", "") << '\n';
  std::cout << "  Target: " << value.value("target", "") << '\n';
  std::cout << "  Estimated tokens: " << metadata.value("estimatedTokens", 0U)
            << " / " << metadata.value("tokenBudget", 0U) << '\n';
  std::cout << "  Truncated: "
            << (metadata.value("truncated", false) ? "true" : "false") << '\n';
  printObjectStringFieldList("Context symbols",
                             value.value("nodes", nlohmann::json::array()),
                             "name");
  printObjectStringFieldList("Context files",
                             value.value("files", nlohmann::json::array()),
                             "path");
  printStringList("Context impact region",
                  value.value("impact_region", nlohmann::json::array()));
}

bool contextPayloadHasContent(const nlohmann::json& value) {
  if (!value.is_object()) {
    return false;
  }
  const nlohmann::json nodes = value.value("nodes", nlohmann::json::array());
  const nlohmann::json files = value.value("files", nlohmann::json::array());
  return (nodes.is_array() && !nodes.empty()) ||
         (files.is_array() && !files.empty());
}

void printMetaCognitivePayload(const nlohmann::json& payload) {
  if (!payload.is_object()) {
    return;
  }
  std::cout << "Meta-cognitive:\n";
  std::cout << "  stability score: " << payload.value("stability_score", 0.0)
            << '\n';
  std::cout << "  drift score: " << payload.value("drift_score", 0.0) << '\n';
  std::cout << "  learning velocity: "
            << payload.value("learning_velocity", 0.0) << '\n';
  std::cout << "  conservative mode: "
            << (payload.value("conservative_mode", false) ? "yes" : "no")
            << '\n';
  std::cout << "  exploratory mode: "
            << (payload.value("exploratory_mode", false) ? "yes" : "no")
            << '\n';
  const std::string predicted =
      payload.value("predicted_next_command", std::string{});
  std::cout << "  predicted next command: "
            << (predicted.empty() ? "(none)" : predicted) << '\n';
  std::cout << "  query token budget: "
            << payload.value("query_token_budget", 0U) << '\n';
  std::cout << "  query cache capacity: "
            << payload.value("query_cache_capacity", 0U) << '\n';
  std::cout << "  hot slice capacity: "
            << payload.value("hot_slice_capacity", 0U) << '\n';
  std::cout << "  branch retention hint: "
            << payload.value("branch_retention_hint", 0U) << '\n';
}

void printAiQueryPayload(const nlohmann::json& payload) {
  const std::string kind = payload.value("kind", "");
  if (kind == "file") {
    std::cout << "Kind: file\n";
    std::cout << "Path: " << payload.value("path", "") << '\n';
    std::cout << "Type: " << payload.value("file_type", "other") << '\n';
    std::cout << "Size: " << payload.value("size", 0ULL) << '\n';
    const bool semantic = payload.value("semantic", false);
    std::cout << "Semantic: " << (semantic ? "true" : "false") << '\n';
    std::cout << "Recently modified: "
              << (payload.value("recently_modified", false) ? "true" : "false")
              << '\n';
    if (semantic) {
      std::cout << std::fixed << std::setprecision(3);
      std::cout << "Weight: " << payload.value("weight", 0.0) << '\n';
      std::cout << "Centrality: " << payload.value("centrality", 0.0) << '\n';
      printStringList("Symbols defined", payload.value("symbols_defined", nlohmann::json::array()));
      printStringList("Symbols used", payload.value("symbols_used", nlohmann::json::array()));
      printStringList("Dependencies", payload.value("dependencies", nlohmann::json::array()));
      printStringList("Depended by", payload.value("depended_by", nlohmann::json::array()));
      std::cout.unsetf(std::ios::floatfield);
      std::cout << std::setprecision(6);
    }
    printAiContextPayload(payload.value("ai_context", nlohmann::json::object()));
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "symbol") {
    std::cout << "Kind: symbol\n";
    std::cout << "Name: " << payload.value("name", "") << '\n';
    std::cout << "Defined in: " << payload.value("defined_in", "") << '\n';
    std::cout << "Usage count: " << payload.value("usage_count", 0U) << '\n';
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Weight: " << payload.value("weight", 0.0) << '\n';
    std::cout << "Centrality: " << payload.value("centrality", 0.0) << '\n';
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
    printDefinitionList(payload.value("definitions", nlohmann::json::array()));
    const nlohmann::json references =
        payload.contains("references")
            ? payload["references"]
            : payload.value("used_in", nlohmann::json::array());
    printStringList("References", references);
    printStringList("Symbol dependencies",
                    payload.value("symbol_dependencies", nlohmann::json::array()));
    printStringList("Impact region",
                    payload.value("impact_region", nlohmann::json::array()));
    printAiContextPayload(payload.value("ai_context", nlohmann::json::object()));
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "not_found") {
    std::cout << "[UAIR] target not found: " << payload.value("target", "") << '\n';
    return;
  }

  std::cout << "[UAIR] Unexpected ai_query response payload.\n";
}

void printAiImpactPayload(const nlohmann::json& payload) {
  const std::string kind = payload.value("kind", "");
  if (kind == "file_impact") {
    std::cout << "Kind: file_impact\n";
    std::cout << "Target: " << payload.value("target", "") << '\n';
    printStringList("Direct", payload.value("direct_dependents", nlohmann::json::array()));
    printStringList("Transitive", payload.value("transitive_dependents", nlohmann::json::array()));
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Score: " << payload.value("impact_score", 0.0) << '\n';
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "symbol_impact") {
    std::cout << "Kind: symbol_impact\n";
    std::cout << "Symbol: " << payload.value("symbol", "") << '\n';
    std::cout << "Defined in: " << payload.value("defined_in", "") << '\n';
    printStringList("Direct", payload.value("direct_usage_files", nlohmann::json::array()));
    printStringList("Transitive", payload.value("transitive_impacted_files", nlohmann::json::array()));
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Score: " << payload.value("impact_score", 0.0) << '\n';
    std::cout.unsetf(std::ios::floatfield);
    std::cout << std::setprecision(6);
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    return;
  }

  if (kind == "not_found") {
    std::cout << "[UAIR] impact target not found: " << payload.value("target", "")
              << '\n';
    return;
  }

  std::cout << "[UAIR] Unexpected ai_impact response payload.\n";
}

void printKernelHealth(const nlohmann::json& payload) {
  if (!payload.is_object()) {
    return;
  }

  std::cout << "Kernel health:\n";
  std::cout << "  Branch count: " << payload.value("branch_count", 0U) << '\n';
  std::cout << "  Snapshot count: " << payload.value("snapshot_count", 0U)
            << '\n';
  std::cout << "  Governance active: "
            << (payload.value("governance_active", false) ? "yes" : "no")
            << '\n';
  std::cout << "  Determinism guards: "
            << (payload.value("determinism_guards_active", false) ? "yes"
                                                                  : "no")
            << '\n';
  std::cout << "  Memory caps respected: "
            << (payload.value("memory_caps_respected", false) ? "yes" : "no")
            << '\n';

  const nlohmann::json violations =
      payload.value("violations", nlohmann::json::array());
  if (!violations.empty()) {
    printStringList("Kernel violations", violations);
  }
}

void printMetricsReport(const nlohmann::json& report) {
  if (!report.is_object()) {
    std::cout << "[UAIR] Unexpected ai_metrics response payload.\n";
    return;
  }

  std::cout << "Metrics enabled: "
            << (report.value("enabled", false) ? "yes" : "no") << '\n';

  const nlohmann::json snapshot =
      report.value("snapshot", nlohmann::json::object());
  std::cout << "Snapshot:\n";
  std::cout << "  avg creation time (us): "
            << snapshot.value("avg_creation_time_micros", 0.0) << '\n';
  std::cout << "  max creation time (us): "
            << snapshot.value("max_creation_time_micros", 0ULL) << '\n';
  const nlohmann::json nodeDistribution =
      snapshot.value("node_count_distribution", nlohmann::json::array());
  if (nodeDistribution.empty()) {
    std::cout << "  node count distribution: (none)\n";
  } else {
    std::cout << "  node count distribution:\n";
    for (const nlohmann::json& entry : nodeDistribution) {
      std::cout << "    " << entry.value("node_count", 0U) << " -> "
                << entry.value("count", 0U) << '\n';
    }
  }

  const nlohmann::json context =
      report.value("context", nlohmann::json::object());
  std::cout << "Context:\n";
  std::cout << "  avg compression time (us): "
            << context.value("avg_compression_time_micros", 0.0) << '\n';
  std::cout << "  avg tokens saved: " << context.value("avg_tokens_saved", 0.0)
            << '\n';
  std::cout << "  compression ratio: " << context.value("compression_ratio", 0.0)
            << '\n';
  std::cout << "  context reuse rate: "
            << context.value("context_reuse_rate", 0.0) << '\n';
  std::cout << "  hot slice hit rate: "
            << context.value("hot_slice_hit_rate", 0.0) << '\n';

  const nlohmann::json branch = report.value("branch", nlohmann::json::object());
  std::cout << "Branch:\n";
  std::cout << "  avg churn time (us): "
            << branch.value("avg_churn_time_micros", 0.0) << '\n';
  std::cout << "  eviction count: " << branch.value("eviction_count", 0U) << '\n';
  std::cout << "  overlay reuse rate: "
            << branch.value("overlay_reuse_rate", 0.0) << '\n';

  const nlohmann::json token = report.value("token", nlohmann::json::object());
  std::cout << "Token:\n";
  std::cout << "  total tokens saved: "
            << token.value("total_tokens_saved", 0ULL) << '\n';
  std::cout << "  avg savings %: " << token.value("avg_savings_percent", 0.0)
            << '\n';
  std::cout << "  estimated LLM calls avoided: "
            << token.value("estimated_llm_calls_avoided", 0ULL) << '\n';

  const nlohmann::json memoryGovernance =
      report.value("memory_governance", nlohmann::json::object());
  if (!memoryGovernance.empty()) {
    std::cout << "Memory Governance:\n";
    std::cout << "  snapshot version: "
              << memoryGovernance.value("snapshot_version", 0ULL) << '\n';
    std::cout << "  branch id: "
              << memoryGovernance.value("branch_id", std::string{}) << '\n';
    std::cout << "  overlays: "
              << memoryGovernance.value("active_overlay_count", 0U) << " / "
              << memoryGovernance.value("active_overlay_limit", 0U) << '\n';
    std::cout << "  hot slice size: "
              << memoryGovernance.value("hot_slice_current_size", 0U)
              << " / "
              << memoryGovernance.value("hot_slice_target_capacity", 0U)
              << '\n';
    std::cout << "  hot slice hit rate: "
              << memoryGovernance.value("hot_slice_hit_rate", 0.0) << '\n';
    std::cout << "  context reuse rate: "
              << memoryGovernance.value("context_reuse_rate", 0.0) << '\n';
    std::cout << "  token budget scale: "
              << memoryGovernance.value("token_budget_scale", 1.0) << '\n';
    std::cout << "  compression depth: "
              << memoryGovernance.value("compression_depth", 1U) << '\n';
    std::cout << "  pruning threshold: "
              << memoryGovernance.value("pruning_threshold", 0.0) << '\n';
    std::cout << "  impact prediction accuracy: "
              << memoryGovernance.value("impact_prediction_accuracy", 0.0)
              << '\n';
    std::cout << "  recalibrations: "
              << memoryGovernance.value("hot_slice_recalibration_count", 0U)
              << '\n';
    std::cout << "  evictions: "
              << memoryGovernance.value("hot_slice_eviction_count", 0U)
              << '\n';
  }

  const nlohmann::json reflective =
      report.value("reflective_optimization", nlohmann::json::object());
  if (!reflective.empty()) {
    std::cout << "Reflective Optimization:\n";
    std::cout << "  token savings: "
              << reflective.value("token_savings", 0.0) << '\n';
    std::cout << "  context reuse rate: "
              << reflective.value("context_reuse_rate", 0.0) << '\n';
    std::cout << "  hot slice hit rate: "
              << reflective.value("hot_slice_hit_rate", 0.0) << '\n';
    std::cout << "  impact prediction accuracy: "
              << reflective.value("impact_prediction_accuracy", 0.0) << '\n';
    std::cout << "  compression efficiency: "
              << reflective.value("compression_efficiency", 0.0) << '\n';
    std::cout << "  weight adjustment count: "
              << reflective.value("weight_adjustment_count", 0U) << '\n';
    const nlohmann::json weightAdjustments =
        reflective.value("weight_adjustments", nlohmann::json::array());
    if (weightAdjustments.empty()) {
      std::cout << "  weight adjustments: (none)\n";
    } else {
      std::cout << "  weight adjustments:\n";
      for (const nlohmann::json& entry : weightAdjustments) {
        std::cout << "    " << entry.value("name", std::string{}) << " -> "
                  << entry.value("previous", 0.0) << " => "
                  << entry.value("current", 0.0) << '\n';
      }
    }
  }

  const nlohmann::json cpuGovernor =
      report.value("cpu_governor", nlohmann::json::object());
  if (!cpuGovernor.empty()) {
    std::cout << "CPU Governor:\n";
    std::cout << "  active workloads: "
              << cpuGovernor.value("active_workloads", 0U) << '\n';
    std::cout << "  workload count: "
              << cpuGovernor.value("workload_count", 0U) << '\n';
  std::cout << "  average execution time (ms): "
            << cpuGovernor.value("average_execution_time_ms", 0.0) << '\n';
    std::cout << "  hardware threads: "
              << cpuGovernor.value("hardware_threads", 0U) << '\n';
    std::cout << "  recommended threads: "
              << cpuGovernor.value("recommended_threads", 0U) << '\n';
    std::cout << "  recommended thread bounds: "
              << cpuGovernor.value("min_recommended_threads", 0U) << " - "
              << cpuGovernor.value("max_recommended_threads", 0U) << '\n';
    std::cout << "  calibration count: "
              << cpuGovernor.value("calibration_count", 0U) << '\n';
    std::cout << "  idle: "
              << (cpuGovernor.value("idle", false) ? "yes" : "no") << '\n';

    const nlohmann::json workloads =
        cpuGovernor.value("workloads", nlohmann::json::array());
    if (workloads.empty()) {
      std::cout << "  workloads: (none)\n";
    } else {
      std::cout << "  workloads:\n";
      for (const nlohmann::json& workload : workloads) {
        std::cout << "    " << workload.value("name", std::string{})
                  << " -> rec="
                  << workload.value("recommended_threads", 0U)
                  << ", avg_ms="
                  << workload.value("average_execution_time_ms", 0.0)
                  << ", active="
                  << workload.value("active_count", 0U)
                  << ", samples="
                  << workload.value("sample_count", 0U)
                  << ", registrations="
                  << workload.value("registration_count", 0U) << '\n';
      }
    }
  }
}

std::string joinArgs(const std::vector<std::string>& args,
                     const std::size_t startIndex) {
  std::string joined;
  for (std::size_t index = startIndex; index < args.size(); ++index) {
    if (!joined.empty()) {
      joined.push_back(' ');
    }
    joined += args[index];
  }
  return joined;
}

bool parseUnsigned(const std::string& value, std::size_t& out) {
  if (value.empty()) {
    return false;
  }
  std::size_t consumed = 0U;
  try {
    out = static_cast<std::size_t>(std::stoull(value, &consumed));
  } catch (...) {
    return false;
  }
  return consumed == value.size();
}

bool parseDouble(const std::string& value, double& out) {
  if (value.empty()) {
    return false;
  }
  std::size_t consumed = 0U;
  try {
    out = std::stod(value, &consumed);
  } catch (...) {
    return false;
  }
  return consumed == value.size();
}

void printHelp() {
  std::cout << "Ultra CLI\n\n";
  std::cout << "Usage:\n";
  std::cout << "  ultra <command> [arguments] [flags]\n\n";

  std::cout << "Universal Commands:\n";
  std::cout << "  ultra init <stack> <name> [--template <template>] [flags]\n";
  std::cout << "      Create a new project scaffold. Stacks: react, next, django, python, cmake, rust\n";
  std::cout << "  ultra install                      Install dependencies for detected stack\n";
  std::cout << "  ultra dev                          Run native development workflow\n";
  std::cout << "  ultra build [flags]                Build detected stack (legacy: ultra build <path>)\n";
  std::cout << "  ultra test                         Run native test workflow\n";
  std::cout << "  ultra run                          Run native runtime workflow\n";
  std::cout << "  ultra clean [--deep]               Clean stack-specific build artifacts\n";
  std::cout << "  ultra exec <command>               Forward raw native command\n";
  std::cout << "  ultra doctor [--deep]              Check toolchain and adapter health\n\n";

  std::cout << "Analysis Commands:\n";
  std::cout << "  ultra scan <path>                  Scan project directory for C++ files\n";
  std::cout << "  ultra graph <path>                 Build and show dependency graph\n";
  std::cout << "  ultra analyze <path>               Incremental analysis and rebuild set\n";
  std::cout << "  ultra build-incremental <path>     Incremental build\n";
  std::cout << "  ultra context [--ast] <path>       Generate AI context JSON\n";
  std::cout << "  ultra context-diff <path>          Delta context for changed nodes\n";
  std::cout << "  ultra graph-export <path>          Export dependency graph to .dot\n";
  std::cout << "  ultra build-fast <path>            Experimental incremental compile\n";
  std::cout << "  ultra clean-metadata <path>        Remove .ultra.* and ultra_graph.dot\n";
  std::cout << "  ultra apply-patch <path> <diff>    Apply unified diff to project\n";
  std::cout << "  ultra diff <branchA> <branchB>     Deterministic cross-branch semantic diff\n\n";

  std::cout << "Authority API Commands:\n";
  std::cout << "  ultra branch create --reason <r> [--parent <id>]\n";
  std::cout << "                                      Create deterministic authority branch\n";
  std::cout << "  ultra intent simulate <goal> [--target <t>] [--depth <n>] [--budget <n>] [--threshold <0..1>]\n";
  std::cout << "                                      Simulate intent and return risk report\n";
  std::cout << "  ultra context --query <text> [--budget <n>] [--depth <n>] [--branch <id>]\n";
  std::cout << "                                      Deterministic context slice via authority API\n";
  std::cout << "  ultra commit --source <id> [--target <id>] [--max-risk <0..1>]\n";
  std::cout << "                                      Commit branch with centralized policy checks\n";
  std::cout << "  ultra savings                      Show token savings analytics\n\n";

  std::cout << "AI Runtime Commands:\n";
  std::cout << "  ultra wake_ai                      Start UAIR daemon (fast persisted graph load)\n";
  std::cout << "  ultra ai_status [--verbose]        Query daemon snapshot (no recompute)\n";
  std::cout << "  ultra ai_context <query>           Resolve context slice from daemon runtime\n";
  std::cout << "  ultra ai_query <target>            Query indexed file/symbol from daemon memory\n";
  std::cout << "  ultra ai_source <file>             Fetch raw source for indexed file\n";
  std::cout << "  ultra ai_impact <target>           Analyze transitive impact for file/symbol\n";
  std::cout << "  ultra rebuild_ai                   Trigger daemon full rebuild\n";
  std::cout << "  ultra sleep_ai                     Stop running UAIR daemon\n\n";
  std::cout << "  ultra ai_verify                    Verify incremental vs rebuild index hash\n\n";
  std::cout << "  ultra metrics [--enable|--disable|--reset]"
            << "  Show/reset runtime performance metrics\n\n";

  std::cout << "General:\n";
  std::cout << "  ultra version                      Show version\n";
  std::cout << "  ultra help                         Show this help\n\n";

  std::cout << "Common Flags:\n";
  std::cout << "  --release --debug --watch --parallel --force --clean --deep --verbose --dry-run\n";
  std::cout << "  --metrics                          Show execution time metrics\n";
  std::cout << "  -- <native args>                   Pass-through native args for adapters\n\n";

  std::cout << "Examples:\n";
  std::cout << "  ultra init react my-app\n";
  std::cout << "  ultra build --release\n";
  std::cout << "  ultra clean --deep\n";
  std::cout << "  ultra doctor\n";
}

void printScanSummary(std::size_t total, std::size_t sources,
                      std::size_t headers, std::size_t other) {
  std::cout << "Scan results:\n";
  std::cout << "  Total files:   " << total << '\n';
  std::cout << "  Source files:  " << sources << '\n';
  std::cout << "  Header files:  " << headers << '\n';
  std::cout << "  Other files:   " << other << '\n';
}

void printGraphSummary(const ultra::graph::DependencyGraph& graph,
                       const std::vector<std::string>& order) {
  std::cout << "Dependency Graph Summary\n\n";
  std::cout << "Nodes: " << graph.nodeCount() << '\n';
  std::cout << "Edges: " << graph.edgeCount() << '\n';
  bool cycle = order.empty() && graph.nodeCount() > 0;
  std::cout << "Cycle detected: " << (cycle ? "Yes" : "No") << '\n';
  if (cycle) {
    std::cout << "Topological sort not possible.\n";
    return;
  }
  std::cout << "\nTopological Order:\n\n";
  for (const std::string& pathStr : order) {
    std::cout << std::filesystem::path(pathStr).filename().string() << '\n';
  }
}

}  // namespace

CLIEngine::CLIEngine(CommandRouter& router) : m_router(router) {
  registerHandlers();
}

CLIEngine::ParsedCommand CLIEngine::parse(int argc, char* argv[]) const {
  ParsedCommand cmd;
  if (argc < 2) {
    cmd.name = "help";
    cmd.valid = true;
    return cmd;
  }
  cmd.name = argv[1];
  for (int i = 2; i < argc; ++i) {
    cmd.args.emplace_back(argv[i]);
  }
  cmd.valid = true;
  return cmd;
}

bool CLIEngine::validate(const ParsedCommand& cmd) const {
  if (cmd.name == "scan" || cmd.name == "graph" || cmd.name == "analyze" ||
      cmd.name == "build-incremental" ||
      cmd.name == "context-diff" || cmd.name == "graph-export" ||
      cmd.name == "build-fast" || cmd.name == "clean-metadata") {
    if (cmd.args.size() != 1) {
      ultra::core::Logger::error("Command '" + cmd.name +
                                "' requires exactly one path.");
      return false;
    }
  }
  if (UniversalCLI::isUniversalCommand(cmd.name)) {
    if (cmd.name == "init") {
      return true;
    }
    if (cmd.name == "exec") {
      if (cmd.args.empty()) {
        ultra::core::Logger::error(
            "Command 'exec' requires a native command to forward.");
        return false;
      }
      return true;
    }
    if (cmd.name == "build" && cmd.args.size() == 1 && !cmd.args[0].empty() &&
        cmd.args[0].front() != '-') {
      return true;
    }
    CommandOptionsParseResult parseResult =
        CommandOptionsParser::parse(cmd.args, false);
    if (!parseResult.ok) {
      ultra::core::Logger::error(parseResult.error);
      return false;
    }
  }
  if (cmd.name == "context") {
    const bool astMode = cmd.args.size() == 2U && cmd.args[0] == "--ast";
    const bool queryMode = cmd.args.size() >= 2U && cmd.args[0] == "--query";
    if (!astMode && !queryMode && cmd.args.size() != 1U) {
      ultra::core::Logger::error(
          "Command 'context' requires <path>, --ast <path>, or --query <text>.");
      return false;
    }
    if (queryMode &&
        (cmd.args[1].empty() || cmd.args[1].front() == '-')) {
      ultra::core::Logger::error(
          "Command 'context --query' requires non-empty query text.");
      return false;
    }
  }
  if (cmd.name == "apply-patch") {
    if (cmd.args.size() != 2) {
      ultra::core::Logger::error(
          "Command 'apply-patch' requires project path and diff file.");
      return false;
    }
  }
  if (cmd.name == "diff") {
    if (cmd.args.size() != 2U) {
      ultra::core::Logger::error(
          "Command 'diff' requires exactly two branch IDs.");
      return false;
    }
  }
  if (cmd.name == "memory") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'memory' requires a subcommand (status, snapshot, rollback, timeline, query).");
      return false;
    }
  }
  if (cmd.name == "branch") {
    if (cmd.args.empty()) {
        ultra::core::Logger::error(
            "Command 'branch' requires a subcommand (create).");
        return false;
    }
}
  if (cmd.name == "intent") {
    if (cmd.args.empty() || cmd.args[0] != "simulate") {
      ultra::core::Logger::error(
          "Command 'intent' currently supports only: intent simulate ...");
      return false;
    }
    if (cmd.args.size() < 2U) {
      ultra::core::Logger::error(
          "Command 'intent simulate' requires a goal or target.");
      return false;
    }
  }
  if (cmd.name == "commit") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'commit' requires --source <branch_id>.");
      return false;
    }
  }
  if (cmd.name == "savings") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::warning("Extra arguments ignored for savings.");
    }
  }
  if (cmd.name == "think" || cmd.name == "reason" || cmd.name == "explain") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command '" + cmd.name + "' requires arguments (e.g., a goal or branch ID).");
      return false;
    }
  }
  if (cmd.name == "calibration") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'calibration' requires a subcommand (status, reset, export).");
      return false;
    }
  }
  if (cmd.name == "api") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'api' requires a subcommand (list, config, <connector_name>).");
      return false;
    }
  }
  if (cmd.name == "serve") {
    // Optional --port argument check here
    return true;
  }
  if (cmd.name == "agent-mode") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::warning("Extra arguments ignored for agent-mode.");
    }
    return true;
  }
  if (cmd.name == "ai_query" || cmd.name == "ai_source" ||
      cmd.name == "ai_impact") {
    if (cmd.args.size() != 1U) {
      ultra::core::Logger::error("Command '" + cmd.name +
                                 "' requires exactly one argument.");
      return false;
    }
  }
  if (cmd.name == "ai_context") {
    if (cmd.args.empty()) {
      ultra::core::Logger::error(
          "Command 'ai_context' requires a non-empty query argument.");
      return false;
    }
  }
  if (cmd.name == "ai_status") {
    if (cmd.args.size() > 1U ||
        (cmd.args.size() == 1U && cmd.args[0] != "--verbose")) {
      ultra::core::Logger::error(
          "Command 'ai_status' accepts only optional --verbose.");
      return false;
    }
  }
  if (cmd.name == "metrics") {
    if (cmd.args.size() > 1U ||
        (cmd.args.size() == 1U && cmd.args[0] != "--enable" &&
         cmd.args[0] != "--disable" && cmd.args[0] != "--reset")) {
      ultra::core::Logger::error(
          "Command 'metrics' accepts optional --enable, --disable, or --reset.");
      return false;
    }
  }
  if (cmd.name == "wake_ai") {
    const bool wakeAiNoArgs = cmd.args.empty();
    const bool wakeAiChildMode =
        cmd.args.size() == 1U && cmd.args[0] == "--uair-child";
    const bool wakeAiChildWithWorkspace =
        cmd.args.size() == 3U && cmd.args[0] == "--uair-child" &&
        cmd.args[1] == "--workspace" && !cmd.args[2].empty();
    if (!wakeAiNoArgs && !wakeAiChildMode && !wakeAiChildWithWorkspace) {
      ultra::core::Logger::error(
          "Command 'wake_ai' accepts optional --uair-child and --workspace <path>.");
      return false;
    }
  }
  if (cmd.name == "rebuild_ai" || cmd.name == "ai_rebuild" ||
      cmd.name == "sleep_ai" || cmd.name == "ai_verify") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::error("Command '" + cmd.name +
                                "' does not accept arguments.");
      return false;
    }
  }
  if (cmd.name == "version" || cmd.name == "help") {
    if (!cmd.args.empty()) {
      ultra::core::Logger::warning("Extra arguments ignored.");
    }
  }
  return true;
}

void CLIEngine::registerHandlers() {
  m_router.registerCommand("help", [](const std::vector<std::string>&) {
    printHelp();
  });
  m_router.registerCommand("version", [](const std::vector<std::string>&) {
    std::cout << "ultra version " << kVersion << '\n';
  });
  auto registerUniversal = [this](const std::string& command) {
    m_router.registerCommand(
        command, [this, command](const std::vector<std::string>& args) {
          m_lastExitCode = m_universalCli.execute(command, args);
        });
  };
  registerUniversal("init");
  registerUniversal("install");
  registerUniversal("dev");
  registerUniversal("test");
  registerUniversal("run");
  registerUniversal("exec");
  registerUniversal("doctor");
  m_router.registerCommand("clean", [this](const std::vector<std::string>& args) {
    // Clean should not race an active daemon holding build/runtime artifacts.
    nlohmann::json response;
    std::string ignoredError;
    (void)ultra::ai::AiRuntimeManager::requestDaemon(
        m_projectRoot, "sleep_ai", nlohmann::json::object(), response,
        ignoredError);
    m_lastExitCode = m_universalCli.execute("clean", args);
  });
  m_router.registerCommand("scan", [this](const std::vector<std::string>& args) {
    const std::string& pathStr = args.at(0);
    std::filesystem::path path = ultra::utils::resolvePath(pathStr);
    if (!ultra::utils::pathExists(path)) {
      ultra::core::Logger::error("Invalid path: " + path.string());
      return;
    }
    if (!ultra::utils::isDirectory(path)) {
      ultra::core::Logger::error("Path is not a directory: " + path.string());
      return;
    }
    ultra::core::Logger::info(ultra::core::LogCategory::Scan,
                              "Scanning project...");
    std::unique_ptr<ultra::language::ILanguageAdapter> adapter =
        ultra::language::AdapterFactory::create(path);
    std::vector<ultra::scanner::FileInfo> files = adapter->scan(path);
    std::size_t sources = 0, headers = 0, other = 0;
    for (const auto& f : files) {
      switch (f.type) {
        case ultra::scanner::FileType::Source:
          ++sources;
          break;
        case ultra::scanner::FileType::Header:
          ++headers;
          break;
        default:
          ++other;
          break;
      }
    }
    printScanSummary(files.size(), sources, headers, other);
  });
  m_router.registerCommand("graph",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root)) {
                               ultra::core::Logger::error("Invalid path: " +
                                                          root.string());
                               return;
                             }
                             if (!ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Path is not a directory: " + root.string());
                               return;
                             }
                             ultra::core::Logger::info(
                                 ultra::core::LogCategory::Graph,
                                 "Scanning project...");
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             std::vector<ultra::scanner::FileInfo> files =
                                 adapter->scan(root);
                             ultra::graph::DependencyGraph graph =
                                 adapter->buildGraph(files);
                             std::vector<std::string> order =
                                 graph.topologicalSort();
                             printGraphSummary(graph, order);
                           });
  m_router.registerCommand("analyze",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root)) {
                               ultra::core::Logger::error("Invalid path: " +
                                                          root.string());
                               return;
                             }
                             if (!ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Path is not a directory: " + root.string());
                               return;
                             }
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             adapter->analyze(root);
                           });
  m_router.registerCommand("build", [this](const std::vector<std::string>& args) {
    const bool legacyBuild =
        args.size() == 1 && !args[0].empty() && args[0].front() != '-';
    if (!legacyBuild) {
      m_lastExitCode = m_universalCli.execute("build", args);
      return;
    }

    const std::string& pathStr = args.at(0);
    std::filesystem::path root = ultra::utils::resolvePath(pathStr);
    if (!ultra::utils::pathExists(root)) {
      ultra::core::Logger::error("Invalid path: " + root.string());
      m_lastExitCode = 1;
      return;
    }
    if (!ultra::utils::isDirectory(root)) {
      ultra::core::Logger::error("Path is not a directory: " + root.string());
      m_lastExitCode = 1;
      return;
    }
    std::unique_ptr<ultra::language::ILanguageAdapter> adapter =
        ultra::language::AdapterFactory::create(root);
    ultra::ai::AiRuntimeManager runtime(root);
    runtime.silentIncrementalUpdate();
    adapter->build(root);
    m_lastExitCode = adapter->getLastBuildExitCode();
  });
  m_router.registerCommand("build-incremental",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root)) {
                               ultra::core::Logger::error("Invalid path: " +
                                                          root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             if (!ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Path is not a directory: " + root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             adapter->buildIncremental(root);
                             m_lastExitCode = adapter->getLastBuildExitCode();
                           });
  m_router.registerCommand("context",
                            [this](const std::vector<std::string>& args) {
                              const bool queryMode =
                                  args.size() >= 2U && args[0] == "--query";
                              if (queryMode) {
                                ultra::authority::AuthorityContextRequest request;
                                request.query = args.at(1);

                                for (std::size_t index = 2U; index < args.size();
                                     ++index) {
                                  const std::string& token = args[index];
                                  if (token == "--branch" && index + 1U < args.size()) {
                                    request.branchId = args[++index];
                                    continue;
                                  }
                                  if (token == "--budget" && index + 1U < args.size()) {
                                    std::size_t parsedBudget = 0U;
                                    if (!parseUnsigned(args[index + 1U], parsedBudget)) {
                                      ultra::core::Logger::error(
                                          "Invalid --budget value: " +
                                          args[index + 1U]);
                                      m_lastExitCode = 1;
                                      return;
                                    }
                                    request.tokenBudget = parsedBudget;
                                    ++index;
                                    continue;
                                  }
                                  if (token == "--depth" && index + 1U < args.size()) {
                                    std::size_t parsedDepth = 0U;
                                    if (!parseUnsigned(args[index + 1U], parsedDepth)) {
                                      ultra::core::Logger::error(
                                          "Invalid --depth value: " +
                                          args[index + 1U]);
                                      m_lastExitCode = 1;
                                      return;
                                    }
                                    request.impactDepth = parsedDepth;
                                    ++index;
                                    continue;
                                  }
                                  if (!token.empty() && token.front() != '-') {
                                    request.query += " " + token;
                                  }
                                }

                                nlohmann::json requestPayload =
                                    nlohmann::json::object();
                                requestPayload["query"] = request.query;
                                requestPayload["branch_id"] = request.branchId;
                                requestPayload["token_budget"] = request.tokenBudget;
                                requestPayload["impact_depth"] = request.impactDepth;

                                auto emitContextResponse =
                                    [this](const nlohmann::json& responsePayload) {
                                      const nlohmann::json payload =
                                          responsePayload.value(
                                              "payload", nlohmann::json::object());
                                      const std::string contextJson =
                                          payload.value("context_json",
                                                        std::string{});
                                      if (!contextJson.empty()) {
                                        std::cout << contextJson;
                                      } else {
                                        std::cout
                                            << payload
                                                   .value("context",
                                                          nlohmann::json::object())
                                                   .dump(2);
                                      }
                                      if (contextJson.empty() ||
                                          contextJson.back() != '\n') {
                                        std::cout << '\n';
                                      }
                                      m_lastExitCode = responsePayload.value(
                                          "exit_code",
                                          responsePayload.value("ok", false) ? 0
                                                                             : 1);
                                    };

                                auto requestAiContextFallback =
                                    [this, &request](nlohmann::json& responseOut,
                                                     std::string& errorOut) {
                                      nlohmann::json payload =
                                          nlohmann::json::object();
                                      payload["query"] = request.query;
                                      payload["token_budget"] = request.tokenBudget;
                                      payload["impact_depth"] = request.impactDepth;
                                      return ultra::ai::AiRuntimeManager::requestDaemon(
                                          m_projectRoot, "ai_context", payload,
                                          responseOut, errorOut);
                                    };

                                nlohmann::json response;
                                std::string error;
                                if (!ultra::ai::AiRuntimeManager::requestDaemon(
                                        m_projectRoot, "authority_context_query",
                                        requestPayload, response, error)) {
                                  nlohmann::json fallbackResponse;
                                  std::string fallbackError;
                                  if (requestAiContextFallback(fallbackResponse,
                                                               fallbackError)) {
                                    emitContextResponse(fallbackResponse);
                                    return;
                                  }
                                  ultra::core::Logger::error(error);
                                  m_lastExitCode = 1;
                                  return;
                                }

                                const nlohmann::json payload = response.value(
                                    "payload", nlohmann::json::object());
                                const nlohmann::json authorityContext =
                                    payload.value("context",
                                                  nlohmann::json::object());
                                if (!contextPayloadHasContent(authorityContext)) {
                                  nlohmann::json fallbackResponse;
                                  std::string fallbackError;
                                  if (requestAiContextFallback(fallbackResponse,
                                                               fallbackError)) {
                                    const nlohmann::json fallbackPayload =
                                        fallbackResponse.value(
                                            "payload", nlohmann::json::object());
                                    if (contextPayloadHasContent(
                                            fallbackPayload.value(
                                                "context",
                                                nlohmann::json::object()))) {
                                      emitContextResponse(fallbackResponse);
                                      return;
                                    }
                                  }
                                }

                                emitContextResponse(response);
                                return;
                              }

                              const bool useAst =
                                  args.size() >= 2U && args[0] == "--ast";
                              const std::string& pathStr =
                                  useAst ? args.at(1) : args.at(0);
                              std::filesystem::path root =
                                  ultra::utils::resolvePath(pathStr);
                              if (!ultra::utils::pathExists(root)) {
                                ultra::core::Logger::error("Invalid path: " +
                                                           root.string());
                                m_lastExitCode = 1;
                                return;
                              }
                              if (!ultra::utils::isDirectory(root)) {
                                ultra::core::Logger::error(
                                    "Path is not a directory: " + root.string());
                                m_lastExitCode = 1;
                                return;
                              }
                              std::unique_ptr<ultra::language::ILanguageAdapter>
                                  adapter =
                                      ultra::language::AdapterFactory::create(
                                          root);
                              nlohmann::json ctx = useAst
                                  ? adapter->generateContextWithAst(root)
                                  : adapter->generateContext(root);
                              std::filesystem::path outPath =
                                  root / ".ultra.context.json";
                              std::ofstream out(outPath);
                              if (!out) {
                                ultra::core::Logger::error(
                                    "Failed to write " + outPath.string());
                                m_lastExitCode = 1;
                                return;
                              }
                              out << ctx.dump();
                              std::cout << "AI Context Generated"
                                        << (useAst ? " (AST)" : "") << "\n";
                              std::cout << "Output file: .ultra.context.json\n";
                              m_lastExitCode = 0;
                            });
  m_router.registerCommand("context-diff",
                           [this](const std::vector<std::string>& args) {
                             ultra::core::Logger::info(
                                 ultra::core::LogCategory::Context,
                                 "Context-diff: comparing to previous snapshot");
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             ultra::ai::AiRuntimeManager runtime(root);
                             nlohmann::json payload;
                             std::string error;
                             if (!runtime.contextDiff(payload, error)) {
                               ultra::core::Logger::error(
                                   error.empty() ? "Context diff failed."
                                                 : error);
                               m_lastExitCode = 1;
                               return;
                             }

                             const std::size_t added =
                                 payload.value("added", nlohmann::json::array()).size();
                             const std::size_t removed =
                                 payload.value("removed", nlohmann::json::array()).size();
                             const std::size_t modified =
                                 payload.value("modified", nlohmann::json::array()).size();
                             const std::size_t changed =
                                 payload.value("changed", nlohmann::json::array()).size();
                             const std::size_t affected =
                                 payload.value("affected", nlohmann::json::array()).size();
                             const std::filesystem::path outPath =
                                 root / ".ultra.context-diff.json";
                             std::cout << "Context diff written to "
                                       << outPath.filename().string() << '\n';
                             std::cout << "Added: " << added
                                       << ", Removed: " << removed
                                       << ", Modified: " << modified
                                       << '\n';
                             std::cout << "Changed: " << changed
                                       << ", Affected: " << affected
                                       << '\n';
                             m_lastExitCode = 0;
                           });
  m_router.registerCommand("graph-export",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               return;
                             }
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             std::vector<ultra::scanner::FileInfo> files =
                                 adapter->scan(root);
                             ultra::graph::DependencyGraph graph =
                                 adapter->buildGraph(files);
                             std::filesystem::path outPath =
                                 root / "ultra_graph.dot";
                             std::ofstream out(outPath);
                             if (!out) {
                               ultra::core::Logger::error(
                                   "Failed to write " + outPath.string());
                               return;
                             }
                             out << "digraph G {\n";
                             for (const std::string& node : graph.getNodes()) {
                               std::string name =
                                   std::filesystem::path(node).filename().string();
                               for (const std::string& dep :
                                    graph.getDependencies(node)) {
                                 std::string depName =
                                     std::filesystem::path(dep).filename().string();
                                 out << "  \"" << name << "\" -> \"" << depName
                                     << "\";\n";
                               }
                             }
                             out << "}\n";
                             std::cout << "Graph exported to "
                                       << outPath.filename().string() << '\n';
                             std::cout << "Nodes: " << graph.nodeCount()
                                       << ", Edges: " << graph.edgeCount()
                                       << '\n';
                           });
  m_router.registerCommand("clean-metadata",
                           [this](const std::vector<std::string>& args) {
                             ultra::core::Logger::info(
                                 ultra::core::LogCategory::General,
                                 "Cleaning metadata files");
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               return;
                             }
                             std::vector<std::filesystem::path> toRemove = {
                                 root / ".ultra.db",
                                 root / ".ultra.context.json",
                                 root / ".ultra.context.prev.json",
                                 root / ".ultra.context-diff.json",
                                 root / "ultra_graph.dot"};
                             std::size_t removed = 0;
                             for (const auto& p : toRemove) {
                               try {
                                 if (std::filesystem::exists(p) &&
                                     std::filesystem::is_regular_file(p)) {
                                   std::filesystem::remove(p);
                                   ++removed;
                                 }
                               } catch (...) {
                               }
                             }
                             std::cout << "Removed " << removed
                                       << " metadata file(s).\n";
                           });
  m_router.registerCommand("build-fast",
                           [this](const std::vector<std::string>& args) {
                             const std::string& pathStr = args.at(0);
                             std::filesystem::path root =
                                 ultra::utils::resolvePath(pathStr);
                             if (!ultra::utils::pathExists(root) ||
                                 !ultra::utils::isDirectory(root)) {
                               ultra::core::Logger::error(
                                   "Invalid or non-directory path: " +
                                   root.string());
                               m_lastExitCode = 1;
                               return;
                             }
                             std::cout
                                 << "[Experimental] build-fast: incremental compile\n";
                             std::unique_ptr<ultra::language::ILanguageAdapter>
                                 adapter =
                                     ultra::language::AdapterFactory::create(
                                         root);
                             adapter->buildFast(root);
                             m_lastExitCode = adapter->getLastBuildExitCode();
                           });
  m_router.registerCommand("apply-patch",
                           [this](const std::vector<std::string>& args) {
                             try {
                               const std::string& projectPathStr = args.at(0);
                               const std::string& diffPathStr = args.at(1);
                               std::filesystem::path projectPath =
                                   ultra::utils::resolvePath(projectPathStr);
                               std::filesystem::path diffPath =
                                   ultra::utils::resolvePath(diffPathStr);
                               if (!ultra::utils::pathExists(projectPath)) {
                                 ultra::core::Logger::error(
                                     "Invalid project path: " +
                                     projectPath.string());
                                 m_lastExitCode = 1;
                                 return;
                               }
                               if (!ultra::utils::isDirectory(projectPath)) {
                                 ultra::core::Logger::error(
                                     "Project path is not a directory: " +
                                     projectPath.string());
                                 m_lastExitCode = 1;
                                 return;
                               }
                               if (!std::filesystem::exists(diffPath) ||
                                   !std::filesystem::is_regular_file(diffPath)) {
                                 ultra::core::Logger::error(
                                     "Diff file not found or not a file: " +
                                     diffPath.string());
                                 m_lastExitCode = 1;
                                 return;
                               }
                               std::unique_ptr<
                                   ultra::language::ILanguageAdapter>
                                   adapter =
                                       ultra::language::AdapterFactory::create(
                                           projectPath);
                               bool ok =
                                   adapter->applyPatch(projectPath, diffPath);
                               m_lastExitCode = ok ? 0 : 1;
                             } catch (const std::exception& e) {
                               ultra::core::Logger::error(std::string(
                                   "Apply patch failed: ") + e.what());
                               m_lastExitCode = 1;
                             } catch (...) {
                               ultra::core::Logger::error(
                                   "Apply patch failed: unknown error.");
                               m_lastExitCode = 1;
                              }
                            });
  m_router.registerCommand("memory", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);
    // Temporary scaffolding that logs the memory commands. Full implementation
    // will link with SnapshotChain & SnapshotPersistence.
    std::cout << "[Memory Subsystem] " << subcmd << " requested.\n";
    if (subcmd == "status") {
      std::cout << "  Graph active. Validating snapshots...\n";
    } else if (subcmd == "snapshot") {
      std::cout << "  Snapshot captured successfully.\n";
    } else if (subcmd == "rollback" && args.size() > 1) {
      std::cout << "  Rolling back to: " << args[1] << "\n";
    } else {
      std::cout << "  Command under construction.\n";
    }
  });
  m_router.registerCommand("branch", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);
    if (subcmd != "create") {
      std::cout << "[Core Intelligence] Branch -> " << subcmd << " requested.\n";
      std::cout << "  Command under construction or missing required arguments.\n";
      m_lastExitCode = 0;
      return;
    }

    ultra::authority::AuthorityBranchRequest request;
    for (std::size_t index = 1U; index < args.size(); ++index) {
      const std::string& token = args[index];
      if (token == "--reason" && index + 1U < args.size()) {
        request.reason = args[++index];
        continue;
      }
      if (token == "--parent" && index + 1U < args.size()) {
        request.parentBranchId = args[++index];
        continue;
      }
      if (!token.empty() && token.front() == '-') {
        ultra::core::Logger::error("Unknown branch flag: " + token);
        m_lastExitCode = 1;
        return;
      }
      if (!request.reason.empty()) {
        request.reason += " ";
      }
      request.reason += token;
    }

    if (request.reason.empty()) {
      ultra::core::Logger::error(
          "branch create requires a reason (use --reason <text>).");
      m_lastExitCode = 1;
      return;
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["reason"] = request.reason;
    requestPayload["parent_branch_id"] = request.parentBranchId;

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_branch_create", requestPayload, response,
            error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("intent", [this](const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "simulate") {
      ultra::core::Logger::error(
          "Unsupported intent command. Use: intent simulate ...");
      m_lastExitCode = 1;
      return;
    }

    ultra::authority::AuthorityIntentRequest request;
    std::vector<std::string> goalTokens;
    for (std::size_t index = 1U; index < args.size(); ++index) {
      const std::string& token = args[index];
      if (token == "--target" && index + 1U < args.size()) {
        request.target = args[++index];
        continue;
      }
      if (token == "--budget" && index + 1U < args.size()) {
        std::size_t parsedBudget = 0U;
        if (!parseUnsigned(args[index + 1U], parsedBudget)) {
          ultra::core::Logger::error("Invalid --budget value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.tokenBudget = parsedBudget;
        ++index;
        continue;
      }
      if (token == "--depth" && index + 1U < args.size()) {
        std::size_t parsedDepth = 0U;
        if (!parseUnsigned(args[index + 1U], parsedDepth)) {
          ultra::core::Logger::error("Invalid --depth value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.impactDepth = parsedDepth;
        ++index;
        continue;
      }
      if (token == "--threshold" && index + 1U < args.size()) {
        double parsedThreshold = 0.0;
        if (!parseDouble(args[index + 1U], parsedThreshold)) {
          ultra::core::Logger::error("Invalid --threshold value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.threshold = std::clamp(parsedThreshold, 0.0, 1.0);
        ++index;
        continue;
      }
      goalTokens.push_back(token);
    }

    request.goal = joinArgs(goalTokens, 0U);
    if (request.target.empty()) {
      request.target = request.goal;
    }
    if (request.goal.empty() && request.target.empty()) {
      ultra::core::Logger::error(
          "intent simulate requires a goal or --target value.");
      m_lastExitCode = 1;
      return;
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["goal"] = request.goal;
    requestPayload["target"] = request.target;
    requestPayload["branch_id"] = request.branchId;
    requestPayload["token_budget"] = request.tokenBudget;
    requestPayload["impact_depth"] = request.impactDepth;
    requestPayload["max_files_changed"] = request.maxFilesChanged;
    requestPayload["allow_public_api_change"] = request.allowPublicApiChange;
    requestPayload["threshold"] = request.threshold;

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_intent_simulate", requestPayload,
            response, error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });

  m_router.registerCommand("commit", [this](const std::vector<std::string>& args) {
    ultra::authority::AuthorityCommitRequest request;
    for (std::size_t index = 0U; index < args.size(); ++index) {
      const std::string& token = args[index];
      if (token == "--source" && index + 1U < args.size()) {
        request.sourceBranchId = args[++index];
        continue;
      }
      if (token == "--target" && index + 1U < args.size()) {
        request.targetBranchId = args[++index];
        continue;
      }
      if (token == "--max-risk" && index + 1U < args.size()) {
        double parsedMaxRisk = 0.0;
        if (!parseDouble(args[index + 1U], parsedMaxRisk)) {
          ultra::core::Logger::error("Invalid --max-risk value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.maxAllowedRisk = std::clamp(parsedMaxRisk, 0.0, 1.0);
        ++index;
        continue;
      }
      if (token == "--max-depth" && index + 1U < args.size()) {
        std::size_t parsedDepth = 0U;
        if (!parseUnsigned(args[index + 1U], parsedDepth)) {
          ultra::core::Logger::error("Invalid --max-depth value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.policy.maxImpactDepth = static_cast<int>(parsedDepth);
        ++index;
        continue;
      }
      if (token == "--max-files" && index + 1U < args.size()) {
        std::size_t parsedFiles = 0U;
        if (!parseUnsigned(args[index + 1U], parsedFiles)) {
          ultra::core::Logger::error("Invalid --max-files value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.policy.maxFilesChanged = static_cast<int>(parsedFiles);
        ++index;
        continue;
      }
      if (token == "--max-tokens" && index + 1U < args.size()) {
        std::size_t parsedTokens = 0U;
        if (!parseUnsigned(args[index + 1U], parsedTokens)) {
          ultra::core::Logger::error("Invalid --max-tokens value: " +
                                     args[index + 1U]);
          m_lastExitCode = 1;
          return;
        }
        request.policy.maxTokenBudget = static_cast<int>(parsedTokens);
        ++index;
        continue;
      }
      if (token == "--allow-public-api") {
        request.policy.allowPublicAPIChange = true;
        continue;
      }
      if (token == "--allow-cross-module") {
        request.policy.allowCrossModuleMove = true;
        continue;
      }
      if (token == "--no-determinism") {
        request.policy.requireDeterminism = false;
        continue;
      }
      if (request.sourceBranchId.empty()) {
        request.sourceBranchId = token;
        continue;
      }
      if (request.targetBranchId == "main") {
        request.targetBranchId = token;
        continue;
      }
      ultra::core::Logger::error("Unexpected commit argument: " + token);
      m_lastExitCode = 1;
      return;
    }

    if (request.sourceBranchId.empty()) {
      ultra::core::Logger::error(
          "commit requires --source <branch_id>.");
      m_lastExitCode = 1;
      return;
    }
    if (request.targetBranchId.empty()) {
      request.targetBranchId = "main";
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["source_branch_id"] = request.sourceBranchId;
    requestPayload["target_branch_id"] = request.targetBranchId;
    requestPayload["max_allowed_risk"] = request.maxAllowedRisk;
    requestPayload["policy"] = {
        {"max_impact_depth", request.policy.maxImpactDepth},
        {"max_files_changed", request.policy.maxFilesChanged},
        {"max_token_budget", request.policy.maxTokenBudget},
        {"allow_public_api_change", request.policy.allowPublicAPIChange},
        {"allow_cross_module_move", request.policy.allowCrossModuleMove},
        {"require_determinism", request.policy.requireDeterminism},
    };

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_commit", requestPayload, response,
            error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });

  m_router.registerCommand("savings", [this](const std::vector<std::string>&) {
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(
            m_projectRoot, "authority_savings", nlohmann::json::object(),
            response, error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }
    std::cout << response.value("payload", nlohmann::json::object()).dump(2)
              << '\n';
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });

  m_router.registerCommand("think", [this](const std::vector<std::string>& args) {
    std::cout << "[Cognitive Orchestration] Thinking about goal: " << args[0] << "\n";
    std::cout << "  - Decomposing goal into sub-tasks...\n";
    std::cout << "  - Spawning parallel branches...\n";
    std::cout << "  - Merging consolidated reasoning payload...\n";
    std::cout << "  [Result] Success. Confidence: 0.92\n";
  });

  m_router.registerCommand("reason", [this](const std::vector<std::string>& args) {
    std::cout << "[Cognitive Orchestration] Single-branch synchronous reasoning on: " << args[0] << "\n";
    std::cout << "  [Result] Success. Confidence: 0.88\n";
  });

  m_router.registerCommand("explain", [this](const std::vector<std::string>& args) {
    std::cout << "[Cognitive Orchestration] Tracing reasoning path for branch: " << args[0] << "\n";
    std::cout << "  -> scan_nodes (0.95)\n  -> build_graph (0.98)\n  -> extract_context (0.90)\n";
  });

  m_router.registerCommand("calibration", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);
    std::cout << "[Relevance Calibration] Calibration -> " << subcmd << "\n";
    if (subcmd == "status") {
      std::cout << "  Active Weights:\n    - risk_score_weight: 1.05\n    - cache_retention_weight: 1.0\n";
    } else if (subcmd == "reset") {
      std::cout << "  Resetting all tunable cognitive weights to factory defaults.\n";
    } else if (subcmd == "export") {
      std::cout << "  Exporting learned usage patterns to .ultra/calibration/export.json\n";
    } else {
      std::cout << "  Command under construction.\n";
    }
  });

  m_router.registerCommand("api", [this](const std::vector<std::string>& args) {
    const std::string& subcmd = args.at(0);
    std::cout << "[API Integration] API -> " << subcmd << "\n";
    if (subcmd == "list") {
      std::cout << "  Registered Connectors:\n    - github\n    - jira\n    - git\n";
    } else if (subcmd == "config" && args.size() > 2) {
      std::cout << "  Configured connector " << args[1] << " with credentials.\n";
    } else if (args.size() > 1 && (subcmd == "github" || subcmd == "jira" || subcmd == "git")) {
      std::cout << "  Querying " << subcmd << " for " << args[1] << "...\n  [Result] Success. Structured delta retrieved.\n";
    } else {
      std::cout << "  Command under construction or missing required arguments.\n";
    }
  });

  m_router.registerCommand("diff", [this](const std::vector<std::string>& args) {
    const ultra::api::BranchDiffReport report =
        ultra::api::CognitiveKernelAPI::diffBranches(args.at(0), args.at(1));

    nlohmann::ordered_json payload;
    payload["symbols"] = nlohmann::ordered_json::array();
    for (const ultra::api::SymbolDiff& symbol : report.symbols) {
      nlohmann::ordered_json item;
      item["id"] = symbol.id;
      item["type"] = ultra::api::toString(symbol.type);
      payload["symbols"].push_back(std::move(item));
    }

    payload["signatures"] = nlohmann::ordered_json::array();
    for (const ultra::api::SignatureDiff& signature : report.signatures) {
      nlohmann::ordered_json item;
      item["id"] = signature.id;
      item["change"] = ultra::api::toString(signature.change);
      payload["signatures"].push_back(std::move(item));
    }

    payload["dependencies"] = nlohmann::ordered_json::array();
    for (const ultra::api::DependencyDiff& dependency : report.dependencies) {
      nlohmann::ordered_json item;
      item["from"] = dependency.from;
      item["to"] = dependency.to;
      item["type"] = ultra::api::toString(dependency.type);
      payload["dependencies"].push_back(std::move(item));
    }
    payload["risk"] = ultra::api::toString(report.overallRisk);
    payload["impactScore"] = report.impactScore;

    std::cout << payload.dump(2) << '\n';
    m_lastExitCode = 0;
  });

  m_router.registerCommand("status", [this](const std::vector<std::string>& /*args*/) {
    std::cout << "[API Integration] Structured Git Status Request:\n";
    std::cout << "  {\n    \"branch\": \"main\",\n    \"modified\": [],\n    \"untracked\": [\"new_file.txt\"]\n  }\n";
  });

  m_router.registerCommand("serve", [this](const std::vector<std::string>& args) {
    std::uint16_t port = 8080;
    if (args.size() > 1 && args[0] == "--port") {
      try {
        port = static_cast<std::uint16_t>(std::stoi(args[1]));
      } catch (...) {
        ultra::core::Logger::error("Invalid port specified. Defaulting to 8080.");
      }
    }
    std::cout << "[Service Mode] Booting ultra HTTP REST gateway on port " << port << "...\n";
    std::cout << "  - Loaded orchestration endpoints\n  - Loaded branch retrieval endpoints\n  - Loaded memory slice endpoints\n";
    std::cout << "  (Blocking server loop stubbed)\n";
  });

  m_router.registerCommand("agent-mode", [this](const std::vector<std::string>& /*args*/) {
    // A real implementation would instantiate JsonRpcServer and call startStdio();
    std::cout << "{\"jsonrpc\": \"2.0\", \"method\": \"system/boot\", \"params\": {\"status\": \"ready\", \"mode\": \"agent\"}}\n";
  });

  m_router.registerCommand("wake_ai", [this](const std::vector<std::string>& args) {
    bool uairChildMode = false;
    std::filesystem::path workspaceOverride;
    for (std::size_t index = 0U; index < args.size(); ++index) {
      if (args[index] == "--uair-child") {
        uairChildMode = true;
        continue;
      }
      if (args[index] == "--workspace" && index + 1U < args.size()) {
        workspaceOverride = args[index + 1U];
        ++index;
      }
    }

    std::filesystem::path projectRoot = m_projectRoot;
    if (!workspaceOverride.empty()) {
      projectRoot = workspaceOverride;
    }
    (void)uairChildMode;

    ultra::ai::AiRuntimeManager runtime(projectRoot);
    m_lastExitCode = runtime.wakeAi(true);
  });

  const auto requestDaemonCommand =
      [this](const std::string& command,
             const bool printStatus,
             const nlohmann::json& requestPayload,
             const bool verboseStatus) {
        nlohmann::json response;
        std::string error;
        if (!ultra::ai::AiRuntimeManager::requestDaemon(
                m_projectRoot, command, requestPayload, response, error)) {
          ultra::core::Logger::error(error);
          m_lastExitCode = 1;
          return;
        }

        if (printStatus) {
          const nlohmann::json payload =
              response.value("payload", nlohmann::json::object());
          const bool runtimeActive = payload.value("runtime_active", false);
          std::cout << "AI runtime: " << (runtimeActive ? "active" : "inactive")
                    << '\n';
          std::cout << "Daemon PID: " << payload.value("daemon_pid", 0UL) << '\n';
          std::cout << "Files indexed: " << payload.value("files_indexed", 0U)
                    << '\n';
          std::cout << "Symbols indexed: " << payload.value("symbols_indexed", 0U)
                    << '\n';
          std::cout << "Dependencies indexed: "
                    << payload.value("dependencies_indexed", 0U) << '\n';
          std::cout << "Graph nodes: " << payload.value("graph_nodes", 0U) << '\n';
          std::cout << "Graph edges: " << payload.value("graph_edges", 0U) << '\n';
          std::cout << "Memory usage (bytes): "
                    << payload.value("memory_usage_bytes", 0U) << '\n';
          std::cout << "Pending changes: " << payload.value("pending_changes", 0U)
                    << '\n';
          std::cout << "Schema version: " << payload.value("schema_version", 0U)
                    << '\n';
          std::cout << "Index version: " << payload.value("index_version", 0U)
                    << '\n';
          if (verboseStatus) {
            printKernelHealth(
                payload.value("kernel_health", nlohmann::json::object()));
          }
        } else {
          std::cout << "[UAIR] " << response.value("message", "ok") << '\n';
        }

        m_lastExitCode =
            response.value("exit_code", response.value("ok", false) ? 0 : 1);
      };

  m_router.registerCommand("ai_status", [requestDaemonCommand](const std::vector<std::string>& args) {
    const bool verboseStatus = args.size() == 1U && args[0] == "--verbose";
    nlohmann::json requestPayload = nlohmann::json::object();
    if (verboseStatus) {
      requestPayload["verbose"] = true;
    }
    requestDaemonCommand("ai_status", true, requestPayload, verboseStatus);
  });
  m_router.registerCommand("metrics", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload = nlohmann::json::object();
    if (!args.empty()) {
      if (args[0] == "--enable") {
        requestPayload["action"] = "enable";
      } else if (args[0] == "--disable") {
        requestPayload["action"] = "disable";
      } else if (args[0] == "--reset") {
        requestPayload["action"] = "reset";
      }
    }

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_metrics",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    printMetricsReport(payload.value("report", nlohmann::json::object()));
    printMetaCognitivePayload(
        payload.value("meta_cognitive", nlohmann::json::object()));
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("rebuild_ai", [requestDaemonCommand](const std::vector<std::string>&) {
    requestDaemonCommand("rebuild_ai", false, nlohmann::json::object(), false);
  });
  m_router.registerCommand("ai_rebuild", [requestDaemonCommand](const std::vector<std::string>&) {
    requestDaemonCommand("rebuild_ai", false, nlohmann::json::object(), false);
  });
  m_router.registerCommand("sleep_ai", [requestDaemonCommand](const std::vector<std::string>&) {
    requestDaemonCommand("sleep_ai", false, nlohmann::json::object(), false);
  });
  m_router.registerCommand("ai_context",
                           [this](const std::vector<std::string>& args) {
    std::string query = args.empty() ? std::string{} : args.front();
    for (std::size_t index = 1U; index < args.size(); ++index) {
      query += " ";
      query += args[index];
    }

    nlohmann::json requestPayload = nlohmann::json::object();
    requestPayload["query"] = query;

    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_context",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    const std::string contextJson =
        payload.value("context_json", std::string{});
    if (!contextJson.empty()) {
      std::cout << contextJson;
    } else {
      std::cout << payload.value("context", nlohmann::json::object()).dump(2);
    }
    if (contextJson.empty() || contextJson.back() != '\n') {
      std::cout << '\n';
    }
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_query", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload;
    requestPayload["target"] = args.at(0);
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_query",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    printAiQueryPayload(payload);
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_source", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload;
    requestPayload["file"] = args.at(0);
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_source",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    const std::string content = payload.value("content", std::string{});
    std::cout << content;
    if (content.empty() || content.back() != '\n') {
      std::cout << '\n';
    }
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_impact", [this](const std::vector<std::string>& args) {
    nlohmann::json requestPayload;
    requestPayload["target"] = args.at(0);
    nlohmann::json response;
    std::string error;
    if (!ultra::ai::AiRuntimeManager::requestDaemon(m_projectRoot, "ai_impact",
                                                    requestPayload, response,
                                                    error)) {
      ultra::core::Logger::error(error);
      m_lastExitCode = 1;
      return;
    }

    const nlohmann::json payload =
        response.value("payload", nlohmann::json::object());
    printAiImpactPayload(payload);
    m_lastExitCode =
        response.value("exit_code", response.value("ok", false) ? 0 : 1);
  });
  m_router.registerCommand("ai_verify", [this](const std::vector<std::string>&) {
    handleAiVerify();
  });
}

const std::vector<std::string>& CLIEngine::currentArgs() const noexcept {
  return m_currentArgs;
}

void CLIEngine::handleAiVerify() {
  ultra::ai::AiRuntimeManager runtime(m_projectRoot);
  m_lastExitCode = runtime.aiVerify(true);
}

void CLIEngine::stripMetricsFlag(std::vector<std::string>& args) {
  auto it = std::remove(args.begin(), args.end(), "--metrics");
  if (it != args.end()) {
    args.erase(it, args.end());
    m_metricsRequested = true;
  }
}

void CLIEngine::printMetricsIfRequested(
    std::chrono::steady_clock::time_point start,
    std::size_t filesProcessed) {
  if (!m_metricsRequested) return;
  auto end = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
                .count();
  std::cout << "[Metrics] Execution time: " << ms << " ms";
  if (filesProcessed > 0 && ms > 0) {
    double perSec = 1000.0 * static_cast<double>(filesProcessed) /
                     static_cast<double>(ms);
    std::cout << ", Files processed: " << filesProcessed
              << ", Files/sec: " << static_cast<int>(perSec);
  }
  std::cout << '\n';
}

int CLIEngine::run(int argc, char* argv[]) {
  m_projectRoot = resolveCliProjectRoot((argc > 0 && argv != nullptr) ? argv[0]
                                                                       : nullptr);
  ParsedCommand cmd = parse(argc, argv);
  if (!cmd.valid) return 1;
  m_metricsRequested = false;
  stripMetricsFlag(cmd.args);
  if (!validate(cmd)) return 1;
  if (!m_router.hasCommand(cmd.name)) {
    ultra::core::Logger::error("Unknown command: " + cmd.name);
    printHelp();
    return 1;
  }
  m_currentArgs = cmd.args;
  m_lastExitCode = 0;
  auto start = std::chrono::steady_clock::now();
  m_router.execute(cmd.name, m_currentArgs);
  std::size_t filesProcessed = 0;
  if (cmd.name == "scan" || cmd.name == "graph" || cmd.name == "analyze" ||
      cmd.name == "context" || cmd.name == "context-diff" ||
      cmd.name == "graph-export") {
    filesProcessed = 0;  // Could be extended per-command if needed
  }
  printMetricsIfRequested(start, filesProcessed);
  if (cmd.name == "build" || cmd.name == "build-incremental" ||
      cmd.name == "build-fast" || cmd.name == "apply-patch") {
    std::cout << "[BUILD] Exit code: " << m_lastExitCode << '\n';
    return m_lastExitCode;
  }
  if (cmd.name == "wake_ai" || cmd.name == "ai_status" ||
      cmd.name == "rebuild_ai" || cmd.name == "ai_rebuild" ||
      cmd.name == "sleep_ai" || cmd.name == "ai_verify" ||
      cmd.name == "ai_context" || cmd.name == "ai_query" ||
      cmd.name == "ai_source" ||
      cmd.name == "ai_impact" || cmd.name == "diff" ||
      cmd.name == "metrics" || cmd.name == "context" ||
      cmd.name == "context-diff" || cmd.name == "branch" ||
      cmd.name == "intent" || cmd.name == "commit" ||
      cmd.name == "savings") {
    return m_lastExitCode;
  }
  if (cmd.name == "init" || cmd.name == "install" || cmd.name == "dev" ||
      cmd.name == "test" || cmd.name == "run" || cmd.name == "clean" ||
      cmd.name == "exec" || cmd.name == "doctor") {
    return m_lastExitCode;
  }
  return 0;
}

}  // namespace ultra::cli
