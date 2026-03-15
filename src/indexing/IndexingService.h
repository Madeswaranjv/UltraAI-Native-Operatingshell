#pragma once

#include "../ai/IntegrityManager.h"
#include "../core/state_manager.h"
#include "../engine/scanner.h"
#include "../graph/DependencyGraph.h"
#include "../runtime/ContextExtractor.h"
#include "../runtime/query_engine.h"

#include <external/json.hpp>

#include "ultra/runtime/event_types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ultra::indexing {

struct IndexSnapshot {
  std::uint64_t version{0U};
  std::size_t filesIndexed{0U};
  std::size_t symbolsIndexed{0U};
  std::size_t dependenciesIndexed{0U};
};

struct SymbolQueryResult {
  bool found{false};
  nlohmann::json payload;
};

struct ContextSliceResult {
  bool found{false};
  std::size_t estimatedTokens{0U};
  nlohmann::json payload;
};

struct ImpactReport {
  bool found{false};
  nlohmann::json payload;
};

class IndexingService {
 public:
  IndexingService(std::filesystem::path projectRoot,
                  core::StateManager& stateManager);

  bool loadOrBuild(IndexSnapshot& snapshotOut, std::string& error);
  bool buildIndex(const std::filesystem::path& projectRoot,
                  IndexSnapshot& snapshotOut,
                  std::string& error);
  bool rebuild(IndexSnapshot& snapshotOut, std::string& error);
  bool applyMutation(const runtime::DaemonEvent& event,
                     engine::ScanOutput& output,
                     std::string& error);

  [[nodiscard]] bool hasSemanticIndex() const;
  [[nodiscard]] runtime::GraphSnapshot snapshot() const;
  [[nodiscard]] IndexSnapshot indexSnapshot() const;

  [[nodiscard]] SymbolQueryResult querySymbol(const std::string& symbolName) const;
  bool queryTarget(const runtime::GraphSnapshot& snapshot,
                   const std::string& target,
                   nlohmann::json& payloadOut,
                   std::string& error) const;
  bool buildContext(const std::string& query,
                    std::size_t tokenBudget,
                    std::size_t impactDepth,
                    ContextSliceResult& resultOut,
                    std::string& error) const;
  bool analyzeImpact(const std::string& target,
                     std::size_t impactDepth,
                     ImpactReport& reportOut,
                     std::string& error) const;
  bool readSource(const std::string& target,
                  nlohmann::json& payloadOut,
                  std::string& error) const;

  [[nodiscard]] std::string resolveIndexedFilePath(
      const runtime::GraphSnapshot& snapshot,
      const std::string& target) const;

 private:
  static void markRuntimeActive(ai::RuntimeState& state);
  static void mergeScanOutput(engine::ScanOutput& aggregate,
                              const engine::ScanOutput& delta);

  [[nodiscard]] graph::DependencyGraph dependencyGraphFromState(
      const ai::RuntimeState& state) const;
  [[nodiscard]] std::vector<std::string> rebuildSetForChange(
      const ai::RuntimeState& state,
      const std::string& changedPath) const;
  bool persistGraphDelta(const engine::ScanOutput& output,
                         std::string& error) const;
  bool applyIncrementalMutation(const runtime::DaemonEvent& event,
                                engine::ScanOutput& output,
                                std::string& error);

  std::filesystem::path projectRoot_;
  core::StateManager& stateManager_;
  engine::Scanner scanner_;
  runtime::QueryEngine queryEngine_;
};

}  // namespace ultra::indexing
