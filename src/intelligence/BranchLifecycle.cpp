#include "BranchLifecycle.h"
#include "../core/Logger.h"
#include "../metrics/PerformanceMetrics.h"
#include "../runtime/CPUGovernor.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <set>

namespace ultra::intelligence {

namespace {

bool parseSnapshotId(const std::string& value, std::uint64_t& out) {
  if (value.empty()) {
    return false;
  }
  std::size_t consumed = 0U;
  try {
    out = std::stoull(value, &consumed);
  } catch (...) {
    return false;
  }
  return consumed == value.size();
}

std::uint64_t elapsedMicros(
    const std::chrono::steady_clock::time_point startedAt) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - startedAt)
          .count());
}

void recordBranchMetric(const std::string& operation,
                        const std::chrono::steady_clock::time_point startedAt,
                        const std::size_t evictionCount,
                        const std::size_t branchCountBefore,
                        const BranchStore& store) {
  if (!metrics::PerformanceMetrics::isEnabled()) {
    return;
  }
  const std::vector<Branch> branches = store.getAll();
  metrics::BranchMetrics metric;
  metric.operation = operation;
  metric.durationMicros = elapsedMicros(startedAt);
  metric.overlayResidentCount = static_cast<std::size_t>(std::count_if(
      branches.begin(), branches.end(),
      [](const Branch& branch) { return branch.isOverlayResident; }));
  metric.evictionCount = evictionCount;
  metric.branchCountBefore = branchCountBefore;
  metric.branchCountAfter = branches.size();
  metrics::PerformanceMetrics::recordBranchMetric(metric);
}

}  // namespace

BranchLifecycle::BranchLifecycle(BranchStore& store)
    : store_(store),
      chain_(nullptr),
      activeGraph_(nullptr) {

  ownedChain_ = std::make_unique<ultra::memory::SnapshotChain>();
  ownedGraph_ = std::make_unique<ultra::memory::StateGraph>();

  chain_ = ownedChain_.get();
  activeGraph_ = ownedGraph_.get();

  assert(chain_ != nullptr);
  assert(activeGraph_ != nullptr);
}

BranchLifecycle::BranchLifecycle(
    BranchStore& store,
    ultra::memory::SnapshotChain& chain,
    ultra::memory::StateGraph& activeGraph)
    : store_(store),
      chain_(&chain),
      activeGraph_(&activeGraph) {}

void BranchLifecycle::runMemoryGovernance(const std::string& protectedBranchId,
                                          const bool heavyMutation) {
  if (chain_ != nullptr) {
    chain_->enforceRetentionCap();
  }
  enforceActiveBranchCap(protectedBranchId);

  std::vector<Branch> branches = store_.getAll();
  branches.erase(
      std::remove_if(branches.begin(), branches.end(),
                     [](const Branch& branch) {
                       return !branch.isOverlayResident;
                     }),
      branches.end());

  while (branches.size() > kMaxTrackedBranches) {
    std::vector<std::string> protectedIds;
    protectedIds.reserve(branches.size() + 2U);
    if (!protectedBranchId.empty()) {
      protectedIds.push_back(protectedBranchId);
    }
    if (!activeBranchId_.empty()) {
      protectedIds.push_back(activeBranchId_);
    }
    for (const Branch& branch : branches) {
      if (!branch.currentExecutionNodeId.empty()) {
        protectedIds.push_back(branch.branchId);
      }
    }
    std::sort(protectedIds.begin(), protectedIds.end());
    protectedIds.erase(std::unique(protectedIds.begin(), protectedIds.end()),
                       protectedIds.end());

    const std::string candidate = evictionPolicy_.selectEvictionCandidate(
        store_, protectedIds, false);
    if (candidate.empty()) {
      break;
    }
    if (!evictBranch(candidate)) {
      break;
    }
    branches = store_.getAll();
    branches.erase(
        std::remove_if(branches.begin(), branches.end(),
                       [](const Branch& branch) {
                         return !branch.isOverlayResident;
                       }),
        branches.end());
  }

  invalidateStaleSnapshotReferences();
  if (heavyMutation) {
    runtime::CPUGovernor::instance().enterIdle();
    runtime::CPUGovernor::instance().exitIdle();
  }
}

void BranchLifecycle::enforceActiveBranchCap(
    const std::string& protectedBranchId) {
  std::vector<Branch> activeBranches = store_.listByState(BranchState::Active);
  activeBranches.erase(
      std::remove_if(activeBranches.begin(), activeBranches.end(),
                     [](const Branch& branch) {
                       return !branch.isOverlayResident;
                     }),
      activeBranches.end());

  while (activeBranches.size() > kMaxActiveBranches) {
    std::vector<std::string> protectedIds;
    protectedIds.reserve(activeBranches.size() + 2U);
    if (!protectedBranchId.empty()) {
      protectedIds.push_back(protectedBranchId);
    }
    if (!activeBranchId_.empty()) {
      protectedIds.push_back(activeBranchId_);
    }
    for (const Branch& branch : activeBranches) {
      if (!branch.currentExecutionNodeId.empty()) {
        protectedIds.push_back(branch.branchId);
      }
    }
    std::sort(protectedIds.begin(), protectedIds.end());
    protectedIds.erase(
        std::unique(protectedIds.begin(), protectedIds.end()),
        protectedIds.end());

    const std::string candidate = evictionPolicy_.selectEvictionCandidate(
        store_, protectedIds, true);
    if (candidate.empty()) {
      break;
    }
    if (!evictBranch(candidate)) {
      break;
    }
    activeBranches = store_.listByState(BranchState::Active);
    activeBranches.erase(
        std::remove_if(activeBranches.begin(), activeBranches.end(),
                       [](const Branch& branch) {
                         return !branch.isOverlayResident;
                       }),
        activeBranches.end());
  }
}

void BranchLifecycle::invalidateStaleSnapshotReferences() {
  if (chain_ == nullptr) {
    return;
  }

  std::set<std::string> retainedSnapshotIds;
  for (const ultra::memory::StateSnapshot& snapshot : chain_->getHistory()) {
    retainedSnapshotIds.insert(snapshot.snapshotId);
    if (snapshot.id != 0ULL) {
      retainedSnapshotIds.insert(std::to_string(snapshot.id));
    }
  }

  std::vector<Branch> branches = store_.getAll();
  std::sort(branches.begin(), branches.end(),
            [](const Branch& left, const Branch& right) {
              return left.branchId < right.branchId;
            });

  for (Branch& branch : branches) {
    if (branch.memorySnapshotId.empty()) {
      continue;
    }
    if (retainedSnapshotIds.find(branch.memorySnapshotId) !=
        retainedSnapshotIds.end()) {
      continue;
    }
    branch.memorySnapshotId.clear();
    store_.update(branch);
  }
}

bool BranchLifecycle::evictBranch(const std::string& branchId) {
  const auto startedAt = std::chrono::steady_clock::now();
  const std::size_t branchCountBefore = store_.getAll().size();
  Branch victim = store_.get(branchId);
  if (victim.branchId.empty() || !victim.isOverlayResident) {
    return false;
  }

  if (activeBranchId_ == victim.branchId) {
    activeBranchId_.clear();
  }

  victim.isOverlayResident = false;
  if (victim.status == BranchState::Active) {
    victim.status = BranchState::Suspended;
  }

  if (!store_.update(victim)) {
    return false;
  }

  const bool evicted = store_.evictOverlay(victim.branchId);
  if (evicted) {
    ++evictionCount_;
    runtime::CPUGovernor::instance().enterIdle();
    runtime::CPUGovernor::instance().exitIdle();
    recordBranchMetric("evict", startedAt, 1U, branchCountBefore, store_);
  }
  return evicted;
}

Branch BranchLifecycle::create(const std::string& goal) {
  return spawn("", goal);
}

Branch BranchLifecycle::spawn(const std::string& parentId,
                               const std::string& goal) {
  const auto startedAt = std::chrono::steady_clock::now();
  const std::size_t branchCountBefore = store_.getAll().size();

  if (!chain_ || !activeGraph_) {
    ultra::core::Logger::error(
        ultra::core::LogCategory::General,
        "BranchLifecycle not initialized");
    return Branch{};
  }

  Branch b;
  b.parentId = parentId;
  b.parentBranchId = parentId;
  b.goal = goal;
  b.status = BranchState::Active;
  b.isOverlayResident = true;
  b.confidence.stabilityScore = 1.0;

  Branch created = store_.create(b);

  // Deterministic snapshot ID sourced from the store sequence manager.
  const std::uint64_t snapId = store_.reserveSequence();

  ultra::memory::StateSnapshot snap =
      activeGraph_->snapshot(snapId);

  chain_->append(snap);

  created.memorySnapshotId = std::to_string(snapId);
  store_.update(created);

  // Maintain parent-child relationship
  if (!parentId.empty()) {
    Branch parent = store_.get(parentId);
    if (!parent.branchId.empty()) {
      parent.subBranches.push_back(created.branchId);
      std::sort(parent.subBranches.begin(), parent.subBranches.end());
      parent.subBranches.erase(
          std::unique(parent.subBranches.begin(), parent.subBranches.end()),
          parent.subBranches.end());
      store_.update(parent);
    }
  }

  activeBranchId_ = created.branchId;
  runMemoryGovernance(created.branchId, false);
  recordBranchMetric("spawn", startedAt, 0U, branchCountBefore, store_);

  ultra::core::Logger::info(
      ultra::core::LogCategory::General,
      "Spawned branch " + created.branchId + ": " + goal);

  return created;
}

bool BranchLifecycle::suspend(const std::string& branchId) {
  Branch b = store_.get(branchId);
  if (b.branchId.empty() || b.status != BranchState::Active)
    return false;

  b.status = BranchState::Suspended;
  if (!store_.update(b)) {
    return false;
  }
  if (activeBranchId_ == branchId) {
    activeBranchId_.clear();
  }
  runMemoryGovernance(activeBranchId_, false);
  return true;
}

bool BranchLifecycle::resume(const std::string& branchId) {
  const auto startedAt = std::chrono::steady_clock::now();
  const std::size_t branchCountBefore = store_.getAll().size();

  if (!chain_ || !activeGraph_)
    return false;

  Branch b = store_.get(branchId);
  if (b.branchId.empty() || b.status != BranchState::Suspended)
    return false;

  if (b.memorySnapshotId.empty())
    return false;

  std::uint64_t snapId = 0ULL;
  if (!parseSnapshotId(b.memorySnapshotId, snapId)) {
    return false;
  }

  ultra::memory::StateSnapshot snap =
      chain_->getSnapshot(snapId);

  if (snap.id == 0)
    return false;
  const bool wasOverlayResident = b.isOverlayResident;

  activeGraph_->restore(snap);

  b.status = BranchState::Active;
  b.isOverlayResident = true;
  if (!store_.update(b)) {
    return false;
  }
  activeBranchId_ = b.branchId;
  runMemoryGovernance(b.branchId, false);
  metrics::PerformanceMetrics::recordOverlayReuse(wasOverlayResident);
  metrics::PerformanceMetrics::recordSnapshotReuse();
  recordBranchMetric("resume", startedAt, 0U, branchCountBefore, store_);
  return true;
}

bool BranchLifecycle::merge(const std::string& sourceId,
                             const std::string& targetId) {
  const auto startedAt = std::chrono::steady_clock::now();
  const std::size_t branchCountBefore = store_.getAll().size();

  Branch source = store_.get(sourceId);
  Branch target = store_.get(targetId);

  if (source.branchId.empty() || target.branchId.empty())
    return false;

  if (source.status == BranchState::Merged ||
      source.status == BranchState::RolledBack)
    return false;

  source.status = BranchState::Merged;

  if (!store_.update(source) || !store_.update(target)) {
    return false;
  }
  activeBranchId_ = target.branchId;
  runMemoryGovernance(target.branchId, true);
  recordBranchMetric("merge", startedAt, 0U, branchCountBefore, store_);

  ultra::core::Logger::info(
      ultra::core::LogCategory::General,
      "Merged branch " + sourceId + " into " + targetId);

  return true;
}

bool BranchLifecycle::archive(const std::string& branchId) {
  const auto startedAt = std::chrono::steady_clock::now();
  const std::size_t branchCountBefore = store_.getAll().size();
  Branch b = store_.get(branchId);
  if (b.branchId.empty() || b.status == BranchState::Archived)
    return false;

  b.status = BranchState::Archived;
  if (!store_.update(b)) {
    return false;
  }
  if (activeBranchId_ == branchId) {
    activeBranchId_.clear();
  }
  runMemoryGovernance(activeBranchId_, true);
  recordBranchMetric("archive", startedAt, 0U, branchCountBefore, store_);
  return true;
}

bool BranchLifecycle::rollback(const std::string& branchId) {

  if (!chain_ || !activeGraph_)
    return false;

  Branch b = store_.get(branchId);
  if (b.branchId.empty() || b.memorySnapshotId.empty())
    return false;

  std::uint64_t snapId = 0ULL;
  if (!parseSnapshotId(b.memorySnapshotId, snapId)) {
    return false;
  }

  ultra::memory::StateSnapshot snap =
      chain_->getSnapshot(snapId);

  if (snap.id == 0)
    return false;

  if (!chain_->rollback(snapId))
    return false;

  activeGraph_->restore(snap);
  metrics::PerformanceMetrics::recordSnapshotReuse();

  b.status = BranchState::RolledBack;
  if (!store_.update(b)) {
    return false;
  }
  if (activeBranchId_ == branchId) {
    activeBranchId_.clear();
  }
  runMemoryGovernance(activeBranchId_, true);

  ultra::core::Logger::info(
      ultra::core::LogCategory::General,
      "Rolled back branch " + branchId);

  return true;
}

}  // namespace ultra::intelligence
