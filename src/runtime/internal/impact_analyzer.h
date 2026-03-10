#pragma once

// INTERNAL - DO NOT EXPOSE OUTSIDE KERNEL

#include <external/json.hpp>

#include "GraphSnapshot.h"
#include "../../engine/impact/ImpactPredictionEngine.h"
#include "../../engine/query/QueryPlanner.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ultra::runtime {

class ImpactAnalyzer {
 public:
  explicit ImpactAnalyzer(const GraphSnapshot& snapshot);

  [[nodiscard]] nlohmann::json analyzeFileImpact(
      const std::string& file,
      std::size_t maxDepth = std::numeric_limits<std::size_t>::max()) const;
  [[nodiscard]] nlohmann::json analyzeSymbolImpact(
      const std::string& symbol,
      std::size_t maxDepth = std::numeric_limits<std::size_t>::max()) const;

 private:
  static const ai::RuntimeState& requireRuntimeState(const GraphSnapshot& snapshot);
  static std::vector<std::string> filesByDepth(
      const engine::impact::ImpactPrediction& prediction,
      std::size_t minDepth,
      std::size_t maxDepth);
  static std::string rootDefinitionPath(
      const engine::impact::ImpactPrediction& prediction);

  GraphSnapshot snapshot_;
};

inline ImpactAnalyzer::ImpactAnalyzer(const GraphSnapshot& snapshot)
    : snapshot_(snapshot) {}

inline const ai::RuntimeState& ImpactAnalyzer::requireRuntimeState(
    const GraphSnapshot& snapshot) {
  if (!snapshot.runtimeState) {
    throw std::runtime_error(
        "Graph snapshot is missing semantic runtime-state data.");
  }
  return *snapshot.runtimeState;
}

inline std::vector<std::string> ImpactAnalyzer::filesByDepth(
    const engine::impact::ImpactPrediction& prediction,
    const std::size_t minDepth,
    const std::size_t maxDepth) {
  std::vector<std::string> files;
  for (const engine::impact::ImpactedFile& file : prediction.files) {
    if (file.isRoot || file.path.empty() || file.depth < minDepth ||
        file.depth > maxDepth) {
      continue;
    }
    files.push_back(file.path);
  }
  engine::query::QueryPlanner::sortAndDedupe(files);
  return files;
}

inline std::string ImpactAnalyzer::rootDefinitionPath(
    const engine::impact::ImpactPrediction& prediction) {
  for (const engine::impact::ImpactedSymbol& symbol : prediction.symbols) {
    if (symbol.isRoot && !symbol.definedIn.empty()) {
      return symbol.definedIn;
    }
  }
  for (const engine::impact::ImpactedFile& file : prediction.files) {
    if (file.isRoot && !file.path.empty()) {
      return file.path;
    }
  }
  return {};
}

inline nlohmann::json ImpactAnalyzer::analyzeFileImpact(
    const std::string& file,
    const std::size_t maxDepth) const {
  const std::size_t boundedDepth =
      maxDepth == std::numeric_limits<std::size_t>::max()
          ? 2U
          : std::max<std::size_t>(1U, maxDepth);
  engine::impact::ImpactPredictionEngine engine(
      requireRuntimeState(snapshot_), snapshot_.graphStore, snapshot_.version);
  const engine::impact::ImpactPrediction prediction =
      engine.predictFileImpact(file, boundedDepth);
  if (prediction.affectedFiles.empty() && prediction.affectedSymbols.empty()) {
    return nlohmann::json{{"kind", "not_found"}, {"target", file}};
  }

  nlohmann::json result;
  result["kind"] = "file_impact";
  result["target"] = file;
  result["direct_dependents"] = filesByDepth(prediction, 1U, 1U);
  result["transitive_dependents"] = filesByDepth(prediction, 2U, boundedDepth);
  result["affected_symbols"] = prediction.affectedSymbols;
  result["impact_region"] = prediction.impactRegion;
  result["impact_score"] = prediction.risk.score;
  return result;
}

inline nlohmann::json ImpactAnalyzer::analyzeSymbolImpact(
    const std::string& symbol,
    const std::size_t maxDepth) const {
  const std::size_t boundedDepth =
      maxDepth == std::numeric_limits<std::size_t>::max()
          ? 2U
          : std::max<std::size_t>(1U, maxDepth);
  engine::impact::ImpactPredictionEngine engine(
      requireRuntimeState(snapshot_), snapshot_.graphStore, snapshot_.version);
  const engine::impact::ImpactPrediction prediction =
      engine.predictSymbolImpact(symbol, boundedDepth);
  if (prediction.affectedFiles.empty() && prediction.affectedSymbols.empty()) {
    return nlohmann::json{{"kind", "not_found"}, {"target", symbol}};
  }

  nlohmann::json result;
  result["kind"] = "symbol_impact";
  result["symbol"] = symbol;
  result["defined_in"] = rootDefinitionPath(prediction);
  result["direct_usage_files"] = filesByDepth(prediction, 1U, 1U);
  result["transitive_impacted_files"] =
      filesByDepth(prediction, 2U, boundedDepth);
  result["affected_symbols"] = prediction.affectedSymbols;
  result["impact_region"] = prediction.impactRegion;
  result["impact_score"] = prediction.risk.score;
  return result;
}

}  // namespace ultra::runtime
