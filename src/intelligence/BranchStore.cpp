#include "BranchStore.h"
//E:\Projects\Ultra\src\intelligence\BranchStore.cpp
#include "../memory/epoch/EpochGuard.h"
#include "../memory/epoch/EpochManager.h"
#include <cassert>
#include <algorithm>
#include <set>
#include <functional>
#include <unordered_set>

namespace ultra::intelligence {

namespace {

template <typename T>
void deleteSharedPtrHolder(void* holder) {
  delete static_cast<std::shared_ptr<T>*>(holder);
}

template <typename T>
void retireSharedPtr(std::shared_ptr<T>&& ptr) {
  if (!ptr) {
    return;
  }
  auto* holder = new std::shared_ptr<T>(std::move(ptr));
  ultra::memory::epoch::EpochManager::instance().retire(
      static_cast<void*>(holder), &deleteSharedPtrHolder<T>);
}

}  // namespace

Branch BranchStore::normalizeParentIds(Branch branch) {
  if (branch.parentId.empty() && !branch.parentBranchId.empty()) {
    branch.parentId = branch.parentBranchId;
  } else if (branch.parentBranchId.empty() && !branch.parentId.empty()) {
    branch.parentBranchId = branch.parentId;
  }
  return branch;
} // namespace

BranchStore::BranchStore() : seqManager_(), idGenerator_() {}

void BranchStore::touchLru(const std::string& branchId) const {
  // Move branchId to front of lruOrder_, creating index entry if needed.
  auto it = lruIndex_.find(branchId);
  if (it != lruIndex_.end()) {
    lruOrder_.erase(it->second);
    lruOrder_.push_front(branchId);
    lruIndex_[branchId] = lruOrder_.begin();
  } else {
    lruOrder_.push_front(branchId);
    lruIndex_[branchId] = lruOrder_.begin();
  }
}

void BranchStore::eraseLru(const std::string& branchId) const {
  auto it = lruIndex_.find(branchId);
  if (it == lruIndex_.end()) {
    return;
  }
  lruOrder_.erase(it->second);
  lruIndex_.erase(it);
}

void BranchStore::add(const Branch& branch) {
  create(branch);
}
Branch BranchStore::create(const Branch& branch) {

  Branch b = normalizeParentIds(branch);

  // If branchId already provided, respect it (needed for tests)
  if (b.branchId.empty()) {
    uint64_t counter = seqManager_.nextIdCounter();
    b.branchId = idGenerator_.generate(b.parentId, b.goal, counter);
  }

  // Assign deterministic sequences if not set
  if (b.creationSequence == 0)
    b.creationSequence = seqManager_.nextSequence();

  b.isOverlayResident = true;
  b.lastMutationSequence = b.creationSequence;

  auto& slot = overlay_[b.branchId];
  retireSharedPtr(std::move(slot));
  slot = std::make_shared<Branch>(b);
  touchLru(b.branchId);

  return *slot;
}

void BranchStore::insertLoaded(const Branch& b) {
  // Insert into base_ as canonical persisted state. Do not touch sequences/counters/LRU.
  Branch persisted = normalizeParentIds(b);
  persisted.isOverlayResident = false;
  auto& slot = base_[persisted.branchId];
  retireSharedPtr(std::move(slot));
  slot = std::make_shared<const Branch>(persisted);
}



Branch BranchStore::get(const std::string& branchId) const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  // Check overlay first
  auto oit = overlay_.find(branchId);
  if (oit != overlay_.end()) {
    if (oit->second->isOverlayResident) {
      touchLru(branchId);
    } else {
      eraseLru(branchId);
    }
    return normalizeParentIds(*oit->second);
  }
  auto bit = base_.find(branchId);
  if (bit != base_.end()) {
    if (bit->second->isOverlayResident) {
      touchLru(branchId);
    } else {
      eraseLru(branchId);
    }
    return normalizeParentIds(*bit->second);
  }
  return Branch{};
}

bool BranchStore::update(const Branch& branch) {
  // Ensure branch exists in either overlay or base
  auto bit = base_.find(branch.branchId);
  auto oit = overlay_.find(branch.branchId);
  if (bit == base_.end() && oit == overlay_.end()) return false;

  // Clone current representation (base or overlay) and replace fields deterministically
  std::shared_ptr<Branch> cloned;
  if (oit != overlay_.end()) cloned = std::make_shared<Branch>(*oit->second);
  else cloned = std::make_shared<Branch>(*bit->second);

  *cloned = normalizeParentIds(branch);
  cloned->lastMutationSequence = seqManager_.nextSequence();

  if (cloned->isOverlayResident) {
    auto& slot = overlay_[cloned->branchId];
    retireSharedPtr(std::move(slot));
    slot = cloned;
    touchLru(cloned->branchId);
  } else {
    auto& baseSlot = base_[cloned->branchId];
    retireSharedPtr(std::move(baseSlot));
    baseSlot = std::make_shared<const Branch>(*cloned);
    auto overlayIt = overlay_.find(cloned->branchId);
    if (overlayIt != overlay_.end()) {
      retireSharedPtr(std::move(overlayIt->second));
      overlay_.erase(overlayIt);
    }
    eraseLru(cloned->branchId);
  }
  return true;
}

std::vector<Branch> BranchStore::listByState(BranchState state) const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  std::vector<Branch> result;
  // Merge keys from base_ and overlay_ deterministically
  std::set<std::string> keys;
  for (const auto& [k, v] : base_) keys.insert(k);
  for (const auto& [k, v] : overlay_) keys.insert(k);
  for (const auto& k : keys) {
    auto b = branchSnapshot(k);
    if (b && b->status == state) result.push_back(*b);
  }
  return result;
}

std::vector<Branch> BranchStore::getChildren(const std::string& parentId) const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  std::vector<Branch> result;
  std::set<std::string> keys;
  for (const auto& [k, v] : base_) keys.insert(k);
  for (const auto& [k, v] : overlay_) keys.insert(k);

  for (const auto& id : keys) {
    auto b = branchSnapshot(id);
    if (b && b->parentId == parentId) result.push_back(*b);
  }
  return result;
}

std::vector<Branch> BranchStore::getTree(const std::string& rootId) const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  std::vector<Branch> result;
  std::unordered_set<std::string> visited;
  std::function<void(const std::string&)> dfs = [&](const std::string& id) {
    if (visited.count(id)) return; // prevent cycles
    visited.insert(id);
    auto b = branchSnapshot(id);
    if (!b) return;
    result.push_back(*b);
    auto children = getChildren(id);
    // Sort children by branchId for deterministic traversal
    std::sort(children.begin(), children.end(), [](const Branch& a, const Branch& b) {
      return a.branchId < b.branchId;
    });
    for (const auto& c : children) dfs(c.branchId);
  };

  dfs(rootId);
  return result;
}

std::vector<Branch> BranchStore::getAll() const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  std::vector<Branch> result;
  std::set<std::string> keys;
  for (const auto& [k, v] : base_) keys.insert(k);
  for (const auto& [k, v] : overlay_) keys.insert(k);
  for (const auto& k : keys) {
    if (auto b = branchSnapshot(k)) result.push_back(*b);
  }
  return result;
}

bool BranchStore::remove(const std::string& branchId) {
  bool erased = false;
  auto overlayIt = overlay_.find(branchId);
  if (overlayIt != overlay_.end()) {
    retireSharedPtr(std::move(overlayIt->second));
    overlay_.erase(overlayIt);
    erased = true;
  }
  auto baseIt = base_.find(branchId);
  if (baseIt != base_.end()) {
    retireSharedPtr(std::move(baseIt->second));
    base_.erase(baseIt);
    erased = true;
  }
  // Remove from LRU structures if present
  eraseLru(branchId);
  return erased;
}

bool BranchStore::evictOverlay(const std::string& branchId) {
  const std::optional<Branch> snapshot = branchSnapshot(branchId);
  if (!snapshot.has_value()) {
    return false;
  }

  Branch preserved = normalizeParentIds(snapshot.value());
  preserved.isOverlayResident = false;
  auto& baseSlot = base_[branchId];
  retireSharedPtr(std::move(baseSlot));
  baseSlot = std::make_shared<const Branch>(preserved);
  auto overlayIt = overlay_.find(branchId);
  if (overlayIt != overlay_.end()) {
    retireSharedPtr(std::move(overlayIt->second));
    overlay_.erase(overlayIt);
  }
  eraseLru(branchId);
  return true;
}

void BranchStore::clear() {
  for (auto& [id, branch] : base_) {
    (void)id;
    retireSharedPtr(std::move(branch));
  }
  for (auto& [id, branch] : overlay_) {
    (void)id;
    retireSharedPtr(std::move(branch));
  }
  base_.clear();
  overlay_.clear();
  lruOrder_.clear();
  lruIndex_.clear();
}

uint64_t BranchStore::getGlobalSequence() const { return seqManager_.getGlobalSequence(); }
uint64_t BranchStore::getIdCounter() const { return seqManager_.getIdCounter(); }
std::vector<std::string> BranchStore::getLruOrder() const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  std::vector<std::string> out;
  out.reserve(lruOrder_.size());
  for (const auto& id : lruOrder_) out.push_back(id);
  return out;
}

void BranchStore::setGlobalSequence(uint64_t seq) { seqManager_.setGlobalSequence(seq); }
void BranchStore::setIdCounter(uint64_t counter) { seqManager_.setIdCounter(counter); }
void BranchStore::setLruOrder(const std::vector<std::string>& order) {
  lruOrder_.clear();
  lruIndex_.clear();
  for (const auto& id : order) {
    lruOrder_.push_back(id);
    auto it = lruOrder_.end(); --it;
    lruIndex_[id] = it;
  }
}

std::vector<std::string> BranchStore::branchIdSnapshot() const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  std::vector<std::string> ids;
  std::set<std::string> keys;
  for (const auto& [k, v] : base_) keys.insert(k);
  for (const auto& [k, v] : overlay_) keys.insert(k);
  for (const auto& k : keys) ids.push_back(k);
  return ids;
}

std::optional<Branch> BranchStore::branchSnapshot(const std::string& id) const {
  ultra::memory::epoch::EpochGuard guard(
      ultra::memory::epoch::EpochManager::instance());
  auto oit = overlay_.find(id);
  if (oit != overlay_.end()) return *oit->second;
  auto bit = base_.find(id);
  if (bit != base_.end()) return *bit->second;
  return std::nullopt;
}

uint64_t BranchStore::reserveIdCounter() { return seqManager_.nextIdCounter(); }
uint64_t BranchStore::reserveSequence() { return seqManager_.nextSequence(); }
std::string BranchStore::generateDeterministicId(const std::string& parentId, const std::string& goal) {
  return idGenerator_.generate(parentId, goal, seqManager_.nextIdCounter());
}

}  // namespace ultra::intelligence
