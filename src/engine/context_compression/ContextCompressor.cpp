#include "ContextCompressor.h"

#include "../context/TokenBudgetManager.h"
#include "../../runtime/CPUGovernor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ultra::engine::context_compression {

namespace {

struct CompressedNode {
  std::uint64_t id{0U};
  std::string name;
  std::string definedIn;
  std::size_t distance{0U};
  double weight{0.0};
  std::size_t dependencyCount{0U};
  std::size_t referenceCount{0U};
  std::size_t impactFileCount{0U};
  std::uint32_t lineNumber{0U};
  bool isRoot{false};
};

struct CompressedFile {
  std::string path;
  std::string module;
  std::vector<std::uint64_t> symbolIds;
  std::size_t distance{0U};
  std::size_t dependencyCount{0U};
  bool isRoot{false};
};

std::string normalizePathToken(const std::string& value) {
  if (value.empty()) {
    return {};
  }

  std::string normalized = value;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');
  normalized =
      std::filesystem::path(normalized).lexically_normal().generic_string();
  if (normalized == ".") {
    return {};
  }
  if (normalized.size() >= 2U && normalized[0] == '.' && normalized[1] == '/') {
    normalized.erase(0, 2U);
  }
  return normalized;
}

std::string modulePathForFile(const std::string& filePath) {
  const std::string normalized = normalizePathToken(filePath);
  if (normalized.empty()) {
    return ".";
  }

  const std::string module =
      std::filesystem::path(normalized).parent_path().generic_string();
  return module.empty() ? "." : module;
}

double roundFixed3(const double value) {
  if (!std::isfinite(value)) {
    return 0.0;
  }
  return std::round(value * 1000.0) / 1000.0;
}

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

bool compactItemFields(nlohmann::ordered_json& items,
                       const bool rootItems,
                       const std::vector<const char*>& fields) {
  if (!items.is_array()) {
    return false;
  }

  for (auto& item : items) {
    if (!item.is_object() || item.value("is_root", false) != rootItems) {
      continue;
    }

    for (const char* field : fields) {
      if (eraseField(item, field)) {
        return true;
      }
    }
  }

  return false;
}

bool compactHierarchy(nlohmann::ordered_json& hierarchy) {
  if (!hierarchy.is_object() || !hierarchy.contains("repository") ||
      !hierarchy["repository"].is_object()) {
    return false;
  }

  if (clearArrayField(hierarchy["repository"], "impact_region")) {
    return true;
  }

  if (!hierarchy["repository"].contains("modules") ||
      !hierarchy["repository"]["modules"].is_array()) {
    return false;
  }

  for (auto& module : hierarchy["repository"]["modules"]) {
    if (!module.is_object()) {
      continue;
    }
    if (eraseField(module, "symbol_ids")) {
      return true;
    }
    if (eraseField(module, "root_file_paths")) {
      return true;
    }
    if (eraseField(module, "file_paths")) {
      return true;
    }
  }

  return false;
}

bool compactSummary(nlohmann::ordered_json& payload) {
  if (!payload.is_object() || !payload.contains("summary") ||
      !payload["summary"].is_object()) {
    return false;
  }

  nlohmann::ordered_json& summary = payload["summary"];
  if (summary.contains("snapshot") && summary["snapshot"].is_object()) {
    if (eraseField(summary["snapshot"], "hash")) {
      return true;
    }
  }
  if (eraseField(summary, "root_dependencies")) {
    return true;
  }
  if (summary.contains("roots") && summary["roots"].is_object()) {
    if (clearArrayField(summary["roots"], "files")) {
      return true;
    }
    if (clearArrayField(summary["roots"], "symbols")) {
      return true;
    }
  }
  if (summary.contains("snapshot") && summary["snapshot"].is_object()) {
    if (eraseField(summary["snapshot"], "branch")) {
      return true;
    }
  }
  return eraseField(payload, "summary");
}

std::string snapshotHashPrefix(const runtime::GraphSnapshot& snapshot) {
  try {
    const std::string hash = snapshot.deterministicHash();
    return hash.size() > 16U ? hash.substr(0, 16U) : hash;
  } catch (...) {
    return {};
  }
}

std::map<std::uint64_t, std::string> buildDefinedInIndex(
    const runtime::GraphSnapshot& snapshot) {
  std::map<std::uint64_t, std::string> definedInById;

  if (snapshot.graph) {
    for (const memory::StateNode& node :
         snapshot.graph->queryByType(memory::NodeType::Symbol)) {
      if (!node.data.is_object()) {
        continue;
      }

      const std::uint64_t symbolId = node.data.value("symbol_id", 0ULL);
      const std::string definedIn =
          normalizePathToken(node.data.value("defined_in", std::string{}));
      if (symbolId == 0U || definedIn.empty()) {
        continue;
      }

      const auto it = definedInById.find(symbolId);
      if (it == definedInById.end() || definedIn < it->second) {
        definedInById[symbolId] = definedIn;
      }
    }
  }

  if (!snapshot.runtimeState) {
    return definedInById;
  }

  std::map<std::uint32_t, std::string> filePathById;
  for (const ai::FileRecord& file : snapshot.runtimeState->files) {
    const std::string path = normalizePathToken(file.path);
    if (path.empty()) {
      continue;
    }

    const auto it = filePathById.find(file.fileId);
    if (it == filePathById.end() || path < it->second) {
      filePathById[file.fileId] = path;
    }
  }

  for (const ai::SymbolRecord& symbol : snapshot.runtimeState->symbols) {
    const auto fileIt = filePathById.find(symbol.fileId);
    if (symbol.symbolId == 0U || fileIt == filePathById.end()) {
      continue;
    }

    const auto it = definedInById.find(symbol.symbolId);
    if (it == definedInById.end() || fileIt->second < it->second) {
      definedInById[symbol.symbolId] = fileIt->second;
    }
  }

  return definedInById;
}

std::vector<CompressedNode> collectNodes(const runtime::GraphSnapshot& snapshot,
                                         const context::ContextSlice& slice) {
  const std::map<std::uint64_t, std::string> definedInById =
      buildDefinedInIndex(snapshot);

  std::vector<CompressedNode> nodes;
  if (!slice.payload.is_object() || !slice.payload.contains("nodes") ||
      !slice.payload["nodes"].is_array()) {
    return nodes;
  }

  for (const auto& item : slice.payload["nodes"]) {
    if (!item.is_object()) {
      continue;
    }

    CompressedNode node;
    node.id = item.value("id", 0ULL);
    node.name = item.value("name", std::string{});
    node.definedIn = normalizePathToken(item.value("defined_in", std::string{}));
    if (node.definedIn.empty() && node.id != 0U) {
      const auto definedIt = definedInById.find(node.id);
      if (definedIt != definedInById.end()) {
        node.definedIn = definedIt->second;
      }
    }
    node.distance = item.value("distance", 0U);
    node.weight = item.value("weight", 0.0);
    node.dependencyCount =
        item.contains("dependencies") && item["dependencies"].is_array()
            ? item["dependencies"].size()
            : 0U;
    node.referenceCount =
        item.contains("references") && item["references"].is_array()
            ? item["references"].size()
            : 0U;
    node.impactFileCount =
        item.contains("impact_files") && item["impact_files"].is_array()
            ? item["impact_files"].size()
            : 0U;
    node.isRoot = item.value("is_root", false);

    if (item.contains("definitions") && item["definitions"].is_array() &&
        !item["definitions"].empty() && item["definitions"][0].is_object()) {
      node.lineNumber = item["definitions"][0].value("line_number", 0U);
    }

    nodes.push_back(std::move(node));
  }

  std::sort(nodes.begin(), nodes.end(),
            [](const CompressedNode& left, const CompressedNode& right) {
              if (left.id != right.id) {
                return left.id < right.id;
              }
              return left.name < right.name;
            });
  return nodes;
}

std::vector<CompressedFile> collectFiles(const context::ContextSlice& slice,
                                         const std::vector<CompressedNode>& nodes) {
  std::map<std::string, CompressedFile> filesByPath;

  if (slice.payload.is_object() && slice.payload.contains("files") &&
      slice.payload["files"].is_array()) {
    for (const auto& item : slice.payload["files"]) {
      if (!item.is_object()) {
        continue;
      }

      const std::string path = normalizePathToken(item.value("path", std::string{}));
      if (path.empty()) {
        continue;
      }

      CompressedFile& file = filesByPath[path];
      file.path = path;
      file.module = modulePathForFile(path);
      file.distance = item.value("distance", 0U);
      file.dependencyCount =
          item.contains("dependencies") && item["dependencies"].is_array()
              ? item["dependencies"].size()
              : 0U;
      file.isRoot = file.isRoot || item.value("is_root", false);
    }
  }

  if (slice.payload.is_object() && slice.payload.contains("impact_region") &&
      slice.payload["impact_region"].is_array()) {
    for (const auto& item : slice.payload["impact_region"]) {
      if (!item.is_string()) {
        continue;
      }

      const std::string path = normalizePathToken(item.get<std::string>());
      if (path.empty()) {
        continue;
      }

      CompressedFile& file = filesByPath[path];
      file.path = path;
      file.module = modulePathForFile(path);
    }
  }

  for (const CompressedNode& node : nodes) {
    if (node.definedIn.empty()) {
      continue;
    }

    CompressedFile& file = filesByPath[node.definedIn];
    file.path = node.definedIn;
    file.module = modulePathForFile(node.definedIn);
    file.isRoot = file.isRoot || node.isRoot;
    if (node.id != 0U) {
      file.symbolIds.push_back(node.id);
    }
  }

  std::vector<CompressedFile> files;
  files.reserve(filesByPath.size());
  for (auto& [path, file] : filesByPath) {
    (void)path;
    std::sort(file.symbolIds.begin(), file.symbolIds.end());
    file.symbolIds.erase(
        std::unique(file.symbolIds.begin(), file.symbolIds.end()),
        file.symbolIds.end());
    files.push_back(std::move(file));
  }

  std::sort(files.begin(), files.end(),
            [](const CompressedFile& left, const CompressedFile& right) {
              return left.path < right.path;
            });
  return files;
}

void refreshSlice(context::ContextSlice& slice,
                  const context::TokenBudgetManager& budgetManager) {
  if (!slice.payload.contains("metadata") || !slice.payload["metadata"].is_object()) {
    slice.payload["metadata"] = nlohmann::ordered_json::object();
  }

  slice.json = slice.payload.dump();
  slice.estimatedTokens = budgetManager.estimateTextTokens(slice.json);
  slice.payload["metadata"]["estimatedTokens"] = slice.estimatedTokens;
  slice.json = slice.payload.dump();
  slice.estimatedTokens = budgetManager.estimateTextTokens(slice.json);
  slice.payload["metadata"]["estimatedTokens"] = slice.estimatedTokens;
}

bool compactCompressedSlice(context::ContextSlice& slice,
                            const context::TokenBudgetManager& budgetManager) {
  refreshSlice(slice, budgetManager);
  while (!budgetManager.fits(slice.estimatedTokens)) {
    bool changed = false;

    changed = compactSummary(slice.payload);
    if (!changed && slice.payload.contains("hierarchy")) {
      changed = compactHierarchy(slice.payload["hierarchy"]);
    }
    if (!changed) {
      changed = compactItemFields(slice.payload["files"], false,
                                  {"symbol_ids", "dependency_count", "module",
                                   "distance", "is_root"});
    }
    if (!changed) {
      changed = compactItemFields(slice.payload["nodes"], false,
                                  {"impact_file_count", "reference_count",
                                   "dependency_count", "line_number", "weight",
                                   "distance", "defined_in", "is_root"});
    }
    if (!changed) {
      changed = compactItemFields(slice.payload["files"], true,
                                  {"symbol_ids", "dependency_count", "module",
                                   "distance", "is_root"});
    }
    if (!changed) {
      changed = compactItemFields(slice.payload["nodes"], true,
                                  {"impact_file_count", "reference_count",
                                   "dependency_count", "line_number", "weight",
                                   "distance", "defined_in", "is_root"});
    }
    if (!changed && slice.payload.contains("hierarchy")) {
      changed = eraseField(slice.payload, "hierarchy");
    }
    if (!changed) {
      changed = clearArrayField(slice.payload, "files");
    }
    if (!changed) {
      changed = clearArrayField(slice.payload, "impact_region");
    }
    if (!changed) {
      break;
    }

    slice.payload["metadata"]["truncated"] = true;
    refreshSlice(slice, budgetManager);
  }

  refreshSlice(slice, budgetManager);
  return budgetManager.fits(slice.estimatedTokens);
}

struct ScopedGovernorWorkload {
  runtime::CPUGovernor& governor;
  const char* name;
  std::chrono::steady_clock::time_point start;
  std::size_t recommendedThreads{1U};

  explicit ScopedGovernorWorkload(const char* workloadName)
      : governor(runtime::CPUGovernor::instance()),
        name(workloadName),
        start(std::chrono::steady_clock::now()) {
    governor.registerWorkload(name);
    unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads == 0U) {
      hardwareThreads = 4U;
    }
    recommendedThreads = governor.recommendedThreadCount(
        static_cast<std::size_t>(hardwareThreads), name);
  }

  ~ScopedGovernorWorkload() {
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start)
            .count();
    governor.recordExecutionTime(name, elapsedMs);
  }
};

nlohmann::ordered_json buildNodePayload(
    const std::vector<CompressedNode>& nodes) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::array();
  for (const CompressedNode& node : nodes) {
    nlohmann::ordered_json item;
    if (!node.definedIn.empty()) {
      item["defined_in"] = node.definedIn;
    }
    if (node.dependencyCount > 0U) {
      item["dependency_count"] = node.dependencyCount;
    }
    if (node.distance > 0U) {
      item["distance"] = node.distance;
    }
    item["id"] = node.id;
    if (node.impactFileCount > 0U) {
      item["impact_file_count"] = node.impactFileCount;
    }
    item["is_root"] = node.isRoot;
    if (node.lineNumber > 0U) {
      item["line_number"] = node.lineNumber;
    }
    item["name"] = node.name;
    if (node.referenceCount > 0U) {
      item["reference_count"] = node.referenceCount;
    }
    if (node.weight > 0.0) {
      item["weight"] = roundFixed3(node.weight);
    }
    payload.push_back(std::move(item));
  }
  return payload;
}

nlohmann::ordered_json buildFilePayload(
    const std::vector<CompressedFile>& files) {
  nlohmann::ordered_json payload = nlohmann::ordered_json::array();
  for (const CompressedFile& file : files) {
    nlohmann::ordered_json item;
    if (file.dependencyCount > 0U) {
      item["dependency_count"] = file.dependencyCount;
    }
    if (file.distance > 0U) {
      item["distance"] = file.distance;
    }
    item["is_root"] = file.isRoot;
    if (!file.module.empty()) {
      item["module"] = file.module;
    }
    item["path"] = file.path;
    if (!file.symbolIds.empty()) {
      item["symbol_ids"] = file.symbolIds;
    }
    payload.push_back(std::move(item));
  }
  return payload;
}

}  // namespace

context::ContextSlice ContextCompressor::compressContext(
    const runtime::GraphSnapshot& snapshot,
    context::ContextSlice slice,
    const std::size_t tokenBudget) const {
  context::ContextPlan plan;
  query::SymbolQueryEngine queryEngine;
  return compressContext(
      snapshot, plan, queryEngine, std::move(slice), tokenBudget);
}

context::ContextSlice ContextCompressor::compressContext(
    const runtime::GraphSnapshot& snapshot,
    const context::ContextPlan& plan,
    const query::SymbolQueryEngine& queryEngine,
    context::ContextSlice slice,
    const std::size_t tokenBudget) const {
  ScopedGovernorWorkload workload("context_compression");
  (void)workload.recommendedThreads;

  const context::TokenBudgetManager budgetManager(tokenBudget);

  const std::vector<CompressedNode> nodes = collectNodes(snapshot, slice);
  const std::vector<CompressedFile> files = collectFiles(slice, nodes);
  const nlohmann::ordered_json hierarchy =
      hierarchyBuilder_.buildHierarchy(snapshot, slice);
  const nlohmann::ordered_json summary = summaryGenerator_.generateSummary(
      snapshot, plan, queryEngine, hierarchy, slice, tokenBudget);

  nlohmann::ordered_json payload;
  payload["files"] = buildFilePayload(files);
  payload["hierarchy"] = hierarchy;
  payload["impact_region"] =
      slice.payload.value("impact_region", nlohmann::ordered_json::array());
  payload["kind"] = slice.payload.value("kind", std::string{});

  nlohmann::ordered_json metadata =
      slice.payload.value("metadata", nlohmann::ordered_json::object());
  nlohmann::ordered_json compression;
  compression["enabled"] = true;
  compression["mode"] = "hierarchical_summary";
  const std::string hashPrefix = snapshotHashPrefix(snapshot);
  if (!hashPrefix.empty()) {
    compression["snapshot_hash"] = hashPrefix;
  }
  metadata["compression"] = std::move(compression);
  metadata["estimatedTokens"] = 0U;
  metadata["rawEstimatedTokens"] =
      slice.rawEstimatedTokens == 0U ? slice.estimatedTokens
                                     : slice.rawEstimatedTokens;
  metadata["selectedFileCount"] = files.size();
  metadata["selectedNodeCount"] =
      slice.includedNodes.empty() ? nodes.size() : slice.includedNodes.size();
  metadata["tokenBudget"] = tokenBudget;
  payload["metadata"] = std::move(metadata);

  payload["nodes"] = buildNodePayload(nodes);
  payload["summary"] = summary;
  payload["target"] = slice.payload.value("target", std::string{});

  slice.payload = std::move(payload);
  if (slice.includedNodes.empty()) {
    for (const CompressedNode& node : nodes) {
      if (node.id != 0U) {
        slice.includedNodes.push_back(node.id);
      }
    }
  }

  refreshSlice(slice, budgetManager);
  if (!compactCompressedSlice(slice, budgetManager)) {
    throw std::runtime_error(
        "Token budget too small for deterministic compressed context.");
  }
  return slice;
}

}  // namespace ultra::engine::context_compression
