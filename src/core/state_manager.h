#pragma once

#include "../ai/RuntimeState.h"
#include "../engine/query/SymbolQueryEngine.h"
#include "../engine/weight_engine.h"
#include "../memory/CognitiveMemoryManager.h"
#include "../memory/lru_manager.h"
#include "../memory/SnapshotChain.h"
#include "../runtime/CognitiveState.h"
#include "../runtime/GraphSnapshot.h"
#include "../runtime/StructuralChange.h"
#include "../runtime/precision_invalidation.h"
#include "graph_store/GraphStore.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ultra::core {

struct RuntimeStatusSnapshot {
  bool runtimeActive{false};
  std::size_t filesIndexed{0};
  std::size_t symbolsIndexed{0};
  std::size_t dependenciesIndexed{0};
  std::size_t pendingChanges{0};
  std::size_t weightsTracked{0};
  std::size_t lruTracked{0};
  std::uint32_t schemaVersion{ai::IntegrityManager::kSchemaVersion};
  std::uint32_t indexVersion{ai::IntegrityManager::kIndexVersion};
};

struct MemoryStats {
  std::size_t hotSliceSize{0};
  std::size_t snapshotCount{0};
  std::size_t activeBranchCount{0};
};

struct KernelHealthSnapshot {
  std::size_t branchCount{0};
  std::size_t snapshotCount{0};
  bool governanceActive{false};
  bool determinismGuardsActive{false};
  bool memoryCapsRespected{false};
  bool healthy{false};
  std::vector<std::string> violations;
};

struct KernelMutationOutcome {
  bool applied{false};
  bool rolledBack{false};
  std::uint64_t versionBefore{0U};
  std::uint64_t versionAfter{0U};
  std::string hashBefore;
  std::string hashAfter;
  std::string message;
};

class StateManager {
 public:
  explicit StateManager(
      const std::filesystem::path& projectRoot = std::filesystem::current_path());

  using WriteMutation = std::function<bool(ai::RuntimeState& state,
                                           engine::WeightEngine& weightEngine,
                                           memory::LruManager& lruManager)>;
  using ReadView = std::function<void(const ai::RuntimeState& state,
                                      const engine::WeightEngine& weightEngine,
                                      const memory::LruManager& lruManager)>;

  void replaceState(ai::RuntimeState state);
  void updateCore(const ai::CoreIndex& core);
  void setActiveBranch(const runtime::BranchId& branch);

  [[nodiscard]] ai::RuntimeState snapshotState() const;
  [[nodiscard]] RuntimeStatusSnapshot snapshotStatus(
      std::size_t pendingChanges) const;
  [[nodiscard]] runtime::GraphSnapshot getSnapshot() const;
  [[nodiscard]] runtime::CognitiveState createCognitiveState(
      std::size_t tokenBudget,
      const runtime::RelevanceProfile& weights = {}) const;
  [[nodiscard]] std::vector<engine::query::SymbolDefinition> findDefinition(
      const std::string& symbolName) const;
  [[nodiscard]] std::vector<std::string> findReferences(
      const std::string& symbolName) const;
  [[nodiscard]] std::vector<std::string> findFileDependencies(
      const std::string& filePath) const;
  [[nodiscard]] std::vector<std::string> findSymbolDependencies(
      const std::string& symbolName) const;
  [[nodiscard]] std::vector<std::string> findImpactRegion(
      const std::string& symbolName,
      std::size_t maxDepth = 2U) const;
  [[nodiscard]] std::uint64_t currentVersion() const;
  [[nodiscard]] MemoryStats getMemoryStats() const;
  bool loadPersistedGraph(std::string& error, std::size_t maxChunks = 0U);
  bool persistGraphStore(std::string& error);
  bool persistGraphStoreIncremental(
      const std::vector<std::uint32_t>& touchedFileIds,
      std::string& error);
  [[nodiscard]] memory::CognitiveMemoryManager& cognitiveMemory() noexcept;
  [[nodiscard]] const memory::CognitiveMemoryManager& cognitiveMemory() const
      noexcept;
  [[nodiscard]] KernelHealthSnapshot verifyKernelHealth() const;
  [[nodiscard]] KernelMutationOutcome applyOverlayMutation(
      const runtime::GraphSnapshot& expectedSnapshot,
      const WriteMutation& mutation);
  void ensureSnapshotCurrent(const runtime::GraphSnapshot& snapshot) const;

  void withReadLock(const ReadView& view) const;
  bool withWriteLock(const WriteMutation& mutation);

 private:
  static std::size_t clampTokenBudget(std::size_t tokenBudget);
  void runMemoryGovernanceLocked(bool heavyMutation);

  void applyPrecisionInvalidation(
      runtime::StructuralChangeType changeType,
      const std::vector<runtime::SymbolID>& affectedSymbols);
  void rebuildGraphLocked();

  mutable std::shared_mutex graphMutex_;
  std::shared_ptr<memory::StateGraph> graph_{std::make_shared<memory::StateGraph>()};
  std::uint64_t globalVersion_{0};
  runtime::BranchId activeBranch_{runtime::BranchId::nil()};
  runtime::DiffResult pendingDiffResult_;
  ai::RuntimeState state_;
  engine::WeightEngine weightEngine_;
  memory::LruManager lruManager_;
  memory::LruManager branchOverlayLruManager_;
  memory::SnapshotChain snapshotChain_;
  graph_store::GraphStore graphStore_;
  engine::query::SymbolQueryEngine symbolQueryEngine_;
  mutable memory::CognitiveMemoryManager cognitiveMemoryManager_;
};

}  // namespace ultra::core
