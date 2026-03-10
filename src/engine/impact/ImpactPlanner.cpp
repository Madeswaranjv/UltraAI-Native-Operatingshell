#include "ImpactPlanner.h"

#include "../query/QueryPlanner.h"
//E:\Projects\Ultra\src\engine\impact\ImpactPlanner.cpp
#include <algorithm>

namespace ultra::engine::impact {

namespace {

void sortAndDedupeStrings(std::vector<std::string>& values) {
  query::QueryPlanner::sortAndDedupe(values);
}

std::size_t safeRequestedDepth(const std::size_t requestedDepth) {
  return requestedDepth == 0U ? 1U : requestedDepth;
}

}  // namespace

ImpactPlan ImpactPlanner::planSymbolImpact(
    const std::string& symbolName,
    const query::SymbolQueryEngine& queryEngine,
    const context::ContextBuilder& contextBuilder,
    const std::size_t maxDepth) const {
  ImpactPlan plan;
  plan.targetKind = ImpactTargetKind::Symbol;
  plan.target = symbolName;
  plan.fileDirection = TraversalDirection::Reverse;
  plan.symbolDirection = TraversalDirection::Reverse;
  if (symbolName.empty()) {
    return plan;
  }

  plan.context =
      contextBuilder.buildImpactContext(symbolName, 4096U, {}, maxDepth);
  plan.rootSymbols.push_back(symbolName);
  plan.symbolTraversalSeeds.push_back(symbolName);

  const auto definitions = queryEngine.findDefinition(symbolName);
  for (const query::SymbolDefinition& definition : definitions) {
    if (!definition.filePath.empty()) {
      plan.rootFiles.push_back(definition.filePath);
    }
  }
  if (plan.rootFiles.empty()) {
    plan.rootFiles = extractRootFilePaths(plan.context, symbolName);
  }

  plan.fileTraversalSeeds = queryEngine.findReferences(symbolName);
  plan.boundaryFiles = extractImpactRegion(plan.context);
  plan.boundaryFiles.insert(plan.boundaryFiles.end(),
                            plan.rootFiles.begin(),
                            plan.rootFiles.end());
  plan.boundarySymbols = extractNodeNames(plan.context);
  plan.fileDepth = determineTraversalDepth(plan.context, maxDepth);
  plan.symbolDepth = determineTraversalDepth(plan.context, maxDepth);
  plan.maxFiles =
      determineBoundaryLimit(plan.context, "candidateFileCount", 16U, 2U);
  plan.maxSymbols =
      determineBoundaryLimit(plan.context, "candidateNodeCount", 32U, 2U);

  sortAndDedupeStrings(plan.rootFiles);
  sortAndDedupeStrings(plan.rootSymbols);
  sortAndDedupeStrings(plan.fileTraversalSeeds);
  sortAndDedupeStrings(plan.symbolTraversalSeeds);
  sortAndDedupeStrings(plan.boundaryFiles);
  sortAndDedupeStrings(plan.boundarySymbols);
  return plan;
}

ImpactPlan ImpactPlanner::planFileImpact(
    const std::string& filePath,
    const query::SymbolQueryEngine& queryEngine,
    const context::ContextBuilder& contextBuilder,
    const std::size_t maxDepth) const {
  (void)queryEngine;

  ImpactPlan plan;
  plan.targetKind = ImpactTargetKind::File;
  plan.target = filePath;
  plan.fileDirection = TraversalDirection::Reverse;
  plan.symbolDirection = TraversalDirection::Reverse;

  const std::string resolvedFilePath = contextBuilder.resolveFilePath(filePath);
  if (resolvedFilePath.empty()) {
    return plan;
  }

  plan.context =
      contextBuilder.buildFileContext(resolvedFilePath, 4096U, {}, maxDepth);
  plan.rootFiles.push_back(resolvedFilePath);
  plan.fileTraversalSeeds.push_back(resolvedFilePath);
  plan.rootSymbols = extractNodeNamesForFile(plan.context, resolvedFilePath);
  if (plan.rootSymbols.empty()) {
    plan.rootSymbols = extractNodeNames(plan.context);
  }
  plan.symbolTraversalSeeds = plan.rootSymbols;
  plan.fileDepth = determineTraversalDepth(plan.context, maxDepth);
  plan.symbolDepth =
      std::max<std::size_t>(1U,
                            std::min<std::size_t>(2U,
                                                  determineTraversalDepth(plan.context,
                                                                          maxDepth)));
  plan.maxFiles =
      determineBoundaryLimit(plan.context, "candidateFileCount", 16U, 2U);
  plan.maxSymbols =
      determineBoundaryLimit(plan.context, "candidateNodeCount", 32U, 2U);

  sortAndDedupeStrings(plan.rootFiles);
  sortAndDedupeStrings(plan.rootSymbols);
  sortAndDedupeStrings(plan.fileTraversalSeeds);
  sortAndDedupeStrings(plan.symbolTraversalSeeds);
  return plan;
}

std::size_t ImpactPlanner::determineTraversalDepth(
    const context::ContextSlice& slice,
    const std::size_t requestedDepth) {
  const std::size_t fallbackDepth = safeRequestedDepth(requestedDepth);
  if (!slice.payload.is_object() || !slice.payload.contains("metadata") ||
      !slice.payload["metadata"].is_object()) {
    return fallbackDepth;
  }

  const auto& metadata = slice.payload["metadata"];
  const std::size_t sliceDepth = metadata.value("impactDepth", fallbackDepth);
  return std::max<std::size_t>(1U, sliceDepth);
}

std::size_t ImpactPlanner::determineBoundaryLimit(
    const context::ContextSlice& slice,
    const char* metadataKey,
    const std::size_t minimumLimit,
    const std::size_t multiplier) {
  std::size_t limit = minimumLimit;
  if (slice.payload.is_object() && slice.payload.contains("metadata") &&
      slice.payload["metadata"].is_object()) {
    const auto& metadata = slice.payload["metadata"];
    const std::size_t count = metadata.value(metadataKey, 0U);
    limit = std::max<std::size_t>(minimumLimit, count * multiplier);
  }
  return std::max<std::size_t>(1U, limit);
}

std::vector<std::string> ImpactPlanner::extractImpactRegion(
    const context::ContextSlice& slice) {
  std::vector<std::string> files;
  if (!slice.payload.is_object()) {
    return files;
  }
  const auto& impactRegion =
      slice.payload.value("impact_region", nlohmann::ordered_json::array());
  if (!impactRegion.is_array()) {
    return files;
  }

  for (const auto& item : impactRegion) {
    if (item.is_string()) {
      files.push_back(item.get<std::string>());
    }
  }
  sortAndDedupeStrings(files);
  return files;
}

std::vector<std::string> ImpactPlanner::extractNodeNames(
    const context::ContextSlice& slice) {
  std::vector<std::string> symbols;
  if (!slice.payload.is_object()) {
    return symbols;
  }
  const auto& nodes = slice.payload.value("nodes", nlohmann::ordered_json::array());
  if (!nodes.is_array()) {
    return symbols;
  }

  for (const auto& node : nodes) {
    if (!node.is_object()) {
      continue;
    }
    const std::string name = node.value("name", std::string{});
    if (!name.empty()) {
      symbols.push_back(name);
    }
  }
  sortAndDedupeStrings(symbols);
  return symbols;
}

std::vector<std::string> ImpactPlanner::extractNodeNamesForFile(
    const context::ContextSlice& slice,
    const std::string& filePath) {
  std::vector<std::string> symbols;
  if (!slice.payload.is_object()) {
    return symbols;
  }
  const auto& nodes = slice.payload.value("nodes", nlohmann::ordered_json::array());
  if (!nodes.is_array()) {
    return symbols;
  }

  for (const auto& node : nodes) {
    if (!node.is_object()) {
      continue;
    }
    if (node.value("defined_in", std::string{}) != filePath) {
      continue;
    }
    const std::string name = node.value("name", std::string{});
    if (!name.empty()) {
      symbols.push_back(name);
    }
  }
  sortAndDedupeStrings(symbols);
  return symbols;
}

std::vector<std::string> ImpactPlanner::extractRootFilePaths(
    const context::ContextSlice& slice,
    const std::string& symbolName) {
  std::vector<std::string> files;
  if (!slice.payload.is_object()) {
    return files;
  }
  const auto& nodes = slice.payload.value("nodes", nlohmann::ordered_json::array());
  if (!nodes.is_array()) {
    return files;
  }

  for (const auto& node : nodes) {
    if (!node.is_object()) {
      continue;
    }
    const std::string name = node.value("name", std::string{});
    if (name != symbolName && !node.value("is_root", false)) {
      continue;
    }
    const std::string definedIn = node.value("defined_in", std::string{});
    if (!definedIn.empty()) {
      files.push_back(definedIn);
    }
  }
  sortAndDedupeStrings(files);
  return files;
}

}  // namespace ultra::engine::impact
