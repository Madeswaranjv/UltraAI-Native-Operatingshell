#pragma once

#include "ChangeSimulator.h"
#include "ImpactGraphTraversal.h"
#include "ImpactPlanner.h"
#include "RiskEvaluator.h"
//E:\Projects\Ultra\src\engine\impact\ImpactPredictionEngine.h
#include "../../ai/RuntimeState.h"
#include "../context/ContextBuilder.h"
#include "../query/SymbolQueryEngine.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ultra::core::graph_store {
class GraphStore;
}

namespace ultra::engine::impact {

class ImpactPredictionEngine {
 public:
  explicit ImpactPredictionEngine(ai::RuntimeState state,
                                  core::graph_store::GraphStore* graphStore = nullptr,
                                  std::uint64_t stateVersion = 1U);

  [[nodiscard]] ImpactPrediction predictSymbolImpact(
      const std::string& symbolName,
      std::size_t maxDepth = 2U) const;
  [[nodiscard]] ImpactPrediction predictFileImpact(
      const std::string& filePath,
      std::size_t maxDepth = 2U) const;
  [[nodiscard]] SimulationResult simulateSymbolChange(
      const std::string& symbolName,
      std::size_t maxDepth = 2U) const;
  [[nodiscard]] SimulationResult simulateFileChange(
      const std::string& filePath,
      std::size_t maxDepth = 2U) const;

 private:
  static bool isDefinitionSymbol(ai::SymbolType symbolType);
  static bool isPublicHeaderPath(const std::string& path);
  static std::uint64_t deterministicSymbolId(const std::string& name,
                                             const std::string& definedIn);

  [[nodiscard]] ImpactPrediction buildPrediction(const ImpactPlan& plan) const;
  [[nodiscard]] std::vector<ImpactedFile> collectImpactedFiles(
      const ImpactPlan& plan) const;
  [[nodiscard]] std::vector<ImpactedSymbol> collectImpactedSymbols(
      const ImpactPlan& plan,
      const std::vector<ImpactedFile>& files) const;
  [[nodiscard]] std::vector<std::uint32_t> resolveFileIds(
      const std::vector<std::string>& paths) const;
  [[nodiscard]] std::vector<std::uint64_t> resolveSymbolIds(
      const std::vector<std::string>& symbolNames) const;
  [[nodiscard]] std::vector<std::string> definedSymbolsForFile(
      const std::string& filePath) const;
  [[nodiscard]] ImpactedSymbol buildImpactedSymbol(
      const std::string& symbolName,
      std::size_t depth,
      bool isRoot,
      const std::string& preferredFilePath = {}) const;
  [[nodiscard]] std::vector<std::string> extractAffectedFiles(
      const std::vector<ImpactedFile>& files) const;
  [[nodiscard]] std::vector<std::string> extractAffectedSymbols(
      const std::vector<ImpactedSymbol>& symbols) const;

  ai::RuntimeState state_;
  core::graph_store::GraphStore* graphStore_{nullptr};
  query::SymbolQueryEngine queryEngine_;
  context::ContextBuilder contextBuilder_;
  ImpactPlanner planner_;
  RiskEvaluator riskEvaluator_;
  ChangeSimulator simulator_;

  std::map<std::string, std::uint32_t> fileIdByPath_;
  std::map<std::uint32_t, std::string> filePathById_;
  std::map<std::uint64_t, ai::SymbolRecord> symbolById_;
  std::map<std::string, std::vector<ai::SymbolRecord>> symbolsByName_;
  std::map<std::string, std::vector<std::string>> definedSymbolsByFile_;
  std::map<std::string, std::vector<std::string>> referencedSymbolsByFile_;
  std::map<std::uint32_t, std::vector<std::uint32_t>> fileForwardAdj_;
  std::map<std::uint32_t, std::vector<std::uint32_t>> fileReverseAdj_;
  std::map<std::uint64_t, std::vector<std::uint64_t>> symbolForwardAdj_;
  std::map<std::uint64_t, std::vector<std::uint64_t>> symbolReverseAdj_;
};

}  // namespace ultra::engine::impact
