#include "CommitCoordinator.h"

#include "UltraAuthorityAPI.h"

#include "../intelligence/BranchLifecycle.h"
#include "../intelligence/BranchPersistence.h"
#include "../intelligence/BranchStore.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace ultra::authority {

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

}  // namespace

CommitCoordinator::CommitCoordinator(std::filesystem::path projectRoot)
    : projectRoot_(std::filesystem::absolute(std::move(projectRoot))
                       .lexically_normal()) {}

bool CommitCoordinator::commit(const AuthorityCommitRequest& request,
                               const AuthorityRiskReport& riskReport,
                               std::string& error) const {
  if (request.sourceBranchId.empty() || request.targetBranchId.empty()) {
    error = "Commit request requires both source and target branch IDs.";
    return false;
  }
  if (request.sourceBranchId == request.targetBranchId) {
    error = "Source and target branch IDs must be different.";
    return false;
  }
  if (!riskReport.withinThreshold ||
      riskReport.score > std::clamp(request.maxAllowedRisk, 0.0, 1.0)) {
    error = "Commit rejected: risk threshold not satisfied.";
    return false;
  }
  if (!request.policy.allowPublicAPIChange && riskReport.publicApiChanges > 0U) {
    error = "Commit rejected: policy forbids public API changes.";
    return false;
  }
  if (request.policy.maxImpactDepth > 0 &&
      riskReport.impactDepth >
          static_cast<std::size_t>(request.policy.maxImpactDepth)) {
    error = "Commit rejected: impact depth exceeds policy limit.";
    return false;
  }
  if (request.policy.requireDeterminism && !riskReport.withinThreshold) {
    error = "Commit rejected: deterministic policy gate failed.";
    return false;
  }

  intelligence::BranchStore store;
  intelligence::BranchPersistence persistence(projectRoot_ / ".ultra");
  if (!persistence.load(store)) {
    error = "Commit rejected: unable to load branch store.";
    return false;
  }

  const std::optional<intelligence::Branch> source =
      store.branchSnapshot(request.sourceBranchId);
  const std::optional<intelligence::Branch> target =
      store.branchSnapshot(request.targetBranchId);
  if (!source.has_value() || !target.has_value()) {
    error = "Commit rejected: source or target branch not found.";
    return false;
  }

  std::uint64_t sourceSnapshotId = 0ULL;
  std::uint64_t targetSnapshotId = 0ULL;
  if (!parseSnapshotId(source->memorySnapshotId, sourceSnapshotId) ||
      !parseSnapshotId(target->memorySnapshotId, targetSnapshotId)) {
    error = "Commit rejected: branch snapshot metadata is inconsistent.";
    return false;
  }
  if (sourceSnapshotId == 0ULL || targetSnapshotId == 0ULL) {
    error = "Commit rejected: branch snapshot ID must be non-zero.";
    return false;
  }

  intelligence::BranchLifecycle lifecycle(store);
  if (!lifecycle.merge(request.sourceBranchId, request.targetBranchId)) {
    error = "Commit rejected: BranchLifecycle::merge() failed.";
    return false;
  }

  if (!persistence.save(store)) {
    error = "Commit rejected: unable to persist branch store.";
    return false;
  }

  error.clear();
  return true;
}

}  // namespace ultra::authority
