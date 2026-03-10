#pragma once
//
//E:\Projects\Ultra\src\intelligence\BranchStore.h
#include "Branch.h"
#include "BranchIdGenerator.h"
#include "DeterministicSequence.h"

#include <map>
#include <string>
#include <vector>
#include <list>
#include <memory>
#include <unordered_map>
#include <optional>

namespace ultra::intelligence {

/// In-memory repository of all reasoning branches.
/// Implements immutable base + overlay model, deterministic LRU, and sequence management.
class BranchStore {
 public:
  BranchStore();

  /// Inserts a new branch into the store. BranchStore is the authority for id/sequence assignment.
  /// External code MUST NOT pre-assign branch.branchId; an assertion will guard this in debug builds.
  Branch create(const Branch& branch);

  /// Compatibility alias for create().
  void add(const Branch& branch);

  /// Insert a branch loaded from persistence into the canonical base state.
  /// This MUST NOT generate ids or modify sequences or LRU. Used only during load().
  void insertLoaded(const Branch& b);

  /// Retrieves a branch by ID. Returns empty Branch if not found.
  Branch get(const std::string& branchId) const;

  /// Updates an existing branch. Replace entirely (clone-then-modify semantics).
  bool update(const Branch& branch);

  /// Lists all branches currently in a specific state.
  std::vector<Branch> listByState(BranchState state) const;

  /// Retrieves all direct child branches of a given branch.
  std::vector<Branch> getChildren(const std::string& parentId) const;

  /// Retrieves the entire subtree of branches descending from rootId.
  std::vector<Branch> getTree(const std::string& rootId) const;

  /// Retrieves all branches in the store.
  std::vector<Branch> getAll() const;

  /// Erases a branch entirely from the store.
  bool remove(const std::string& branchId);

  /// Clears the entire store.
  void clear();

  // Metadata accessors used by persistence and eviction policy.
  uint64_t getGlobalSequence() const;
  uint64_t getIdCounter() const;
  std::vector<std::string> getLruOrder() const;

  void setGlobalSequence(uint64_t seq);
  void setIdCounter(uint64_t counter);
  void setLruOrder(const std::vector<std::string>& order);

  // Deterministic ID/sequence reservation (exposed for lifecycle)
  uint64_t reserveIdCounter();
  uint64_t reserveSequence();
  std::string generateDeterministicId(const std::string& parentId, const std::string& goal);

  // Used by eviction policy to retrieve branch snapshots.
  std::vector<std::string> branchIdSnapshot() const;
  std::optional<Branch> branchSnapshot(const std::string& id) const;

  // Evict runtime overlay while preserving structural branch metadata.
  bool evictOverlay(const std::string& branchId);

 private:
  // Immutable base branches (persisted canonical state)
  std::map<std::string, std::shared_ptr<const Branch>> base_;

  // Mutable overlay branches (cloned when modified)
  std::map<std::string, std::shared_ptr<Branch>> overlay_;

  // LRU tracking (front = most recently used)
  mutable std::list<std::string> lruOrder_;
  mutable std::unordered_map<std::string, std::list<std::string>::iterator> lruIndex_;

  // Deterministic id/sequence generation helpers
  DeterministicSequence seqManager_;
  BranchIdGenerator idGenerator_;

  // Helpers
  static Branch normalizeParentIds(Branch branch);
  void touchLru(const std::string& branchId) const;
  void eraseLru(const std::string& branchId) const;
};

}  // namespace ultra::intelligence
