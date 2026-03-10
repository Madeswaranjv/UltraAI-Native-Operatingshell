#include "BranchEvictionPolicy.h"
#include "BranchStore.h"
#include "BranchRetentionCalculator.h"
//BranchEvictionPolicy.cpp
#include <algorithm>
#include <set>
#include <unordered_map>
#include <vector>

namespace ultra::intelligence {

namespace {

struct CandidateScore {
  std::string branchId;
  double retentionScore{0.0};
  std::size_t lruRank{0U};
};

}  // namespace

std::string BranchEvictionPolicy::selectEvictionCandidate(
    const BranchStore& store,
    const std::vector<std::string>& protectedBranchIds,
    const bool activeOnly) const {
  auto ids = store.branchIdSnapshot();
  if (ids.empty()) return std::string();
  std::sort(ids.begin(), ids.end());

  // Determine LRU order snapshot
  auto lru = store.getLruOrder();
  std::unordered_map<std::string, size_t> rank;
  for (size_t i = 0; i < lru.size(); ++i) rank[lru[i]] = i;
  const std::set<std::string> protectedIds(
      protectedBranchIds.begin(),
      protectedBranchIds.end());

  BranchRetentionCalculator calc;
  std::vector<CandidateScore> candidates;
  candidates.reserve(ids.size());

  for (const auto& id : ids) {
    if (protectedIds.find(id) != protectedIds.end()) {
      continue;
    }
    auto ob = store.branchSnapshot(id);
    if (!ob) continue;
    if (!ob->isOverlayResident) {
      continue;
    }
    if (activeOnly && ob->status != BranchState::Active) {
      continue;
    }
    size_t r = rank.count(id) ? rank[id] : lru.size();
    candidates.push_back(
        {id, calc.computeRetentionScore(*ob, r, ids.size()), r});
  }

  if (candidates.empty()) {
    return std::string();
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const CandidateScore& left, const CandidateScore& right) {
              if (left.retentionScore != right.retentionScore) {
                return left.retentionScore < right.retentionScore;
              }
              if (left.lruRank != right.lruRank) {
                return left.lruRank > right.lruRank;
              }
              return left.branchId < right.branchId;
            });

  return candidates.front().branchId;
}

}  // namespace ultra::intelligence
