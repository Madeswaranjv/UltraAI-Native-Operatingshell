#include "ContextHierarchyBuilder.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ultra::engine::context_compression {

namespace {

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

}  // namespace

nlohmann::ordered_json ContextHierarchyBuilder::buildHierarchy(
    const runtime::GraphSnapshot& snapshot,
    const context::ContextSlice& slice) const {
  const std::map<std::uint64_t, std::string> definedInById =
      buildDefinedInIndex(snapshot);

  std::set<std::string> impactRegion;
  std::set<std::string> relevantPaths;
  std::set<std::string> rootPaths;
  std::map<std::string, std::vector<std::uint64_t>> symbolIdsByFile;

  if (slice.payload.is_object()) {
    const auto impactRegionJson =
        slice.payload.value("impact_region", nlohmann::ordered_json::array());
    if (impactRegionJson.is_array()) {
      for (const auto& item : impactRegionJson) {
        if (!item.is_string()) {
          continue;
        }
        const std::string path = normalizePathToken(item.get<std::string>());
        if (path.empty()) {
          continue;
        }
        impactRegion.insert(path);
        relevantPaths.insert(path);
      }
    }

    const auto filesJson =
        slice.payload.value("files", nlohmann::ordered_json::array());
    if (filesJson.is_array()) {
      for (const auto& item : filesJson) {
        if (!item.is_object()) {
          continue;
        }

        const std::string path =
            normalizePathToken(item.value("path", std::string{}));
        if (path.empty()) {
          continue;
        }
        relevantPaths.insert(path);
        if (item.value("is_root", false)) {
          rootPaths.insert(path);
        }
      }
    }

    const auto nodesJson =
        slice.payload.value("nodes", nlohmann::ordered_json::array());
    if (nodesJson.is_array()) {
      for (const auto& item : nodesJson) {
        if (!item.is_object()) {
          continue;
        }

        const std::uint64_t symbolId = item.value("id", 0ULL);
        std::string definedIn =
            normalizePathToken(item.value("defined_in", std::string{}));
        if (definedIn.empty() && symbolId != 0U) {
          const auto definedIt = definedInById.find(symbolId);
          if (definedIt != definedInById.end()) {
            definedIn = definedIt->second;
          }
        }
        if (definedIn.empty()) {
          continue;
        }

        relevantPaths.insert(definedIn);
        if (item.value("is_root", false)) {
          rootPaths.insert(definedIn);
        }
        if (symbolId != 0U) {
          symbolIdsByFile[definedIn].push_back(symbolId);
        }
      }
    }
  }

  struct ModuleAggregate {
    std::set<std::string> filePaths;
    std::set<std::string> rootFilePaths;
    std::set<std::uint64_t> symbolIds;
  };

  std::map<std::string, ModuleAggregate> modulesByPath;
  for (const std::string& path : relevantPaths) {
    ModuleAggregate& module = modulesByPath[modulePathForFile(path)];
    module.filePaths.insert(path);
    if (rootPaths.find(path) != rootPaths.end()) {
      module.rootFilePaths.insert(path);
    }

    const auto symbolIt = symbolIdsByFile.find(path);
    if (symbolIt == symbolIdsByFile.end()) {
      continue;
    }
    module.symbolIds.insert(symbolIt->second.begin(), symbolIt->second.end());
  }

  nlohmann::ordered_json repository;
  nlohmann::ordered_json orderedImpactRegion = nlohmann::ordered_json::array();
  for (const std::string& path : impactRegion) {
    orderedImpactRegion.push_back(path);
  }
  repository["impact_region"] = std::move(orderedImpactRegion);

  nlohmann::ordered_json modules = nlohmann::ordered_json::array();
  for (const auto& [modulePath, aggregate] : modulesByPath) {
    nlohmann::ordered_json module;
    module["file_count"] = aggregate.filePaths.size();

    nlohmann::ordered_json filePaths = nlohmann::ordered_json::array();
    for (const std::string& path : aggregate.filePaths) {
      filePaths.push_back(path);
    }
    module["file_paths"] = std::move(filePaths);
    module["path"] = modulePath;

    nlohmann::ordered_json rootFilePaths = nlohmann::ordered_json::array();
    for (const std::string& path : aggregate.rootFilePaths) {
      rootFilePaths.push_back(path);
    }
    module["root_file_paths"] = std::move(rootFilePaths);
    module["symbol_count"] = aggregate.symbolIds.size();

    nlohmann::ordered_json symbolIds = nlohmann::ordered_json::array();
    for (const std::uint64_t symbolId : aggregate.symbolIds) {
      symbolIds.push_back(symbolId);
    }
    module["symbol_ids"] = std::move(symbolIds);
    modules.push_back(std::move(module));
  }
  repository["modules"] = std::move(modules);

  nlohmann::ordered_json hierarchy;
  hierarchy["repository"] = std::move(repository);
  return hierarchy;
}

}  // namespace ultra::engine::context_compression
