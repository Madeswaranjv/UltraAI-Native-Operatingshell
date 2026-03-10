#include "CognitiveKernelAPI.h"

#include "../diff/DiffEngine.h"
#include "../core/Logger.h"
#include "../intelligence/BranchPersistence.h"
#include "../memory/CognitiveMemoryManager.h"
#include "../memory/HotSlice.h"
#include "../memory/SnapshotPersistence.h"
#include "../metrics/PerformanceMetrics.h"
#include "../runtime/ContextExtractor.h"
#include "../runtime/governance/GovernanceEngine.h"
#include "../runtime/impact_analyzer.h"
#include "../runtime/intent/IntentEvaluator.h"
#include "../runtime/query_engine.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ultra::api {

namespace {

constexpr std::size_t kTokenBudgetCeiling = 1U << 20;

std::vector<std::string> sortedUnique(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

[[noreturn]] void throwContractViolation(const std::string& message) {
  core::Logger::error(core::LogCategory::Context, message);
  throw std::runtime_error(message);
}

void validateCognitiveState(const runtime::CognitiveState& state) {
  if (!state.snapshot.graph) {
    throwContractViolation("Invalid cognitive state: graph snapshot is empty.");
  }
  if (state.snapshot.version == 0U) {
    throwContractViolation("Invalid cognitive state: snapshot version is zero.");
  }
  const std::string branchId = state.snapshot.branch.toString();
  if (branchId.empty()) {
    throwContractViolation(
        "Invalid cognitive state: active branch identifier is empty.");
  }
  if (runtime::BranchId::fromString(branchId) != state.snapshot.branch) {
    throwContractViolation(
        "Invalid cognitive state: branch identifier is inconsistent.");
  }
  if (state.budget == 0U) {
    throwContractViolation("Invalid cognitive state: token budget is zero.");
  }
  if (state.budget > kTokenBudgetCeiling) {
    throwContractViolation("Invalid cognitive state: token budget exceeds ceiling.");
  }
  if (state.branch != state.snapshot.branch) {
    throwContractViolation(
        "Invalid cognitive state: branch does not match pinned snapshot branch.");
  }
  std::string deterministicHash;
  try {
    deterministicHash = state.snapshot.deterministicHash();
  } catch (const std::exception& ex) {
    throwContractViolation(
        std::string("Invalid cognitive state: hash computation failed: ") +
        ex.what());
  } catch (...) {
    throwContractViolation(
        "Invalid cognitive state: hash computation failed.");
  }
  if (deterministicHash.empty()) {
    throwContractViolation("Invalid cognitive state: deterministic hash is empty.");
  }
  if (deterministicHash != state.pinnedHash) {
    throwContractViolation("Invalid cognitive state: pinned snapshot hash mismatch.");
  }
  if (state.pinnedVersion != state.snapshot.version) {
    throwContractViolation("Invalid cognitive state: pinned version mismatch.");
  }

  memory::HotSlice alignedWorkingSet = state.workingSet;
  const std::size_t entryCount = alignedWorkingSet.currentSize();
  const std::vector<memory::StateNode> alignedEntries =
      alignedWorkingSet.getTopK(entryCount, state.snapshot.version);
  if (alignedEntries.size() != entryCount) {
    throwContractViolation(
        "Invalid cognitive state: working set version mismatch.");
  }
}

#ifndef NDEBUG
void validateDeterministicOutput(const nlohmann::json& payload) {
  const auto validate = [&](const auto& self,
                            const nlohmann::json& node) -> void {
    if (node.is_object()) {
      std::string previousKey;
      for (auto it = node.begin(); it != node.end(); ++it) {
        const std::string key = it.key();
        if (!previousKey.empty()) {
          assert(previousKey <= key);
        }
        std::string lowered = key;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](const unsigned char c) {
                         return static_cast<char>(std::tolower(c));
                       });
        assert(lowered.find("timestamp") == std::string::npos);
        assert(lowered.find("random") == std::string::npos);
        assert(lowered.find("nonce") == std::string::npos);
        self(self, it.value());
        previousKey = key;
      }
      return;
    }

    if (!node.is_array()) {
      return;
    }

    bool allStrings = true;
    for (const auto& item : node) {
      if (!item.is_string()) {
        allStrings = false;
        break;
      }
    }
    if (allStrings) {
      std::string previousValue;
      for (const auto& item : node) {
        const std::string value = item.get<std::string>();
        if (!previousValue.empty()) {
          assert(previousValue <= value);
        }
        previousValue = value;
      }
    }

    for (const auto& item : node) {
      self(self, item);
    }
  };

  validate(validate, payload);
}
#else
void validateDeterministicOutput(const nlohmann::json&) {}
#endif

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

std::string resolveImpactTarget(const runtime::CognitiveState& state,
                                const NodeID& id,
                                bool& fileTarget) {
  fileTarget = false;
  if (id.rfind("file:", 0U) == 0U) {
    fileTarget = true;
    return id.substr(5U);
  }

  if (id.rfind("symbol:", 0U) != 0U) {
    return id;
  }

  const std::string raw = id.substr(7U);
  std::size_t consumed = 0U;
  std::uint64_t symbolId = 0ULL;
  try {
    symbolId = std::stoull(raw, &consumed);
  } catch (...) {
    return id;
  }
  if (consumed != raw.size() || !state.snapshot.graph) {
    return id;
  }

  const auto symbols =
      state.snapshot.graph->queryByType(memory::NodeType::Symbol);
  for (const memory::StateNode& node : symbols) {
    if (!node.data.contains("symbol_id")) {
      continue;
    }
    if (node.data.value("symbol_id", 0ULL) != symbolId) {
      continue;
    }
    const std::string name = node.data.value("name", std::string{});
    if (!name.empty()) {
      return name;
    }
  }
  return id;
}

DiffType mapDiffType(const diff::semantic::DiffType type) {
  switch (type) {
    case diff::semantic::DiffType::Added:
      return DiffType::Added;
    case diff::semantic::DiffType::Removed:
      return DiffType::Removed;
    case diff::semantic::DiffType::Renamed:
      return DiffType::Renamed;
    case diff::semantic::DiffType::Moved:
      return DiffType::Moved;
    case diff::semantic::DiffType::Modified:
      return DiffType::Modified;
  }
  return DiffType::Modified;
}

SignatureChange mapSignatureChange(const diff::semantic::SignatureChange change) {
  switch (change) {
    case diff::semantic::SignatureChange::Signature:
      return SignatureChange::Signature;
    case diff::semantic::SignatureChange::Visibility:
      return SignatureChange::Visibility;
    case diff::semantic::SignatureChange::Relocation:
      return SignatureChange::Relocation;
    case diff::semantic::SignatureChange::Rename:
      return SignatureChange::Rename;
  }
  return SignatureChange::Signature;
}

RiskLevel mapRiskLevel(const diff::semantic::RiskLevel level) {
  switch (level) {
    case diff::semantic::RiskLevel::LOW:
      return RiskLevel::LOW;
    case diff::semantic::RiskLevel::MEDIUM:
      return RiskLevel::MEDIUM;
    case diff::semantic::RiskLevel::HIGH:
      return RiskLevel::HIGH;
  }
  return RiskLevel::LOW;
}

bool lessSymbolDiff(const SymbolDiff& left, const SymbolDiff& right) {
  if (left.id != right.id) {
    return left.id < right.id;
  }
  return static_cast<int>(left.type) < static_cast<int>(right.type);
}

bool lessSignatureDiff(const SignatureDiff& left, const SignatureDiff& right) {
  if (left.id != right.id) {
    return left.id < right.id;
  }
  return static_cast<int>(left.change) < static_cast<int>(right.change);
}

bool lessDependencyDiff(const DependencyDiff& left,
                         const DependencyDiff& right) {
  if (left.from != right.from) {
    return left.from < right.from;
  }
  if (left.to != right.to) {
    return left.to < right.to;
  }
  return static_cast<int>(left.type) < static_cast<int>(right.type);
}

runtime::QueryKind mapQueryKind(const QueryKind kind) {
  switch (kind) {
    case QueryKind::Auto:
      return runtime::QueryKind::Auto;
    case QueryKind::Symbol:
      return runtime::QueryKind::Symbol;
    case QueryKind::File:
      return runtime::QueryKind::File;
    case QueryKind::Impact:
      return runtime::QueryKind::Impact;
  }
  return runtime::QueryKind::Auto;
}

runtime::Query toRuntimeQuery(const Query& query) {
  runtime::Query runtimeQuery;
  runtimeQuery.kind = mapQueryKind(query.kind);
  runtimeQuery.target = query.target;
  runtimeQuery.impactDepth = query.impactDepth;
  return runtimeQuery;
}

}  // namespace

std::string CognitiveKernelAPI::getMinimalContext(
    const runtime::CognitiveState& state,
    const Query& query) {
  validateCognitiveState(state);
  runtime::ContextExtractor extractor;
  const std::string contextJson =
      extractor.getMinimalContext(state, toRuntimeQuery(query)).json;
#ifndef NDEBUG
  validateDeterministicOutput(nlohmann::json::parse(contextJson));
#endif
  return contextJson;
}

ImpactRegion CognitiveKernelAPI::getImpactRegion(const runtime::CognitiveState& state,
                                                 NodeID id) {
  validateCognitiveState(state);
  ImpactRegion region;
  region.id = std::move(id);

  bool fileTarget = false;
  const std::string target = resolveImpactTarget(state, region.id, fileTarget);
  try {
    runtime::ImpactAnalyzer analyzer(state.snapshot);
    const nlohmann::json payload = fileTarget
                                       ? analyzer.analyzeFileImpact(target)
                                       : analyzer.analyzeSymbolImpact(target);

    if (fileTarget) {
      if (payload.contains("direct_dependents") &&
          payload["direct_dependents"].is_array()) {
        region.direct =
            sortedUnique(payload["direct_dependents"].get<std::vector<std::string>>());
      }
      if (payload.contains("transitive_dependents") &&
          payload["transitive_dependents"].is_array()) {
        region.transitive = sortedUnique(
            payload["transitive_dependents"].get<std::vector<std::string>>());
      }
    } else {
      if (payload.contains("direct_usage_files") &&
          payload["direct_usage_files"].is_array()) {
        region.direct = sortedUnique(
            payload["direct_usage_files"].get<std::vector<std::string>>());
      }
      if (payload.contains("transitive_impacted_files") &&
          payload["transitive_impacted_files"].is_array()) {
        region.transitive = sortedUnique(
            payload["transitive_impacted_files"].get<std::vector<std::string>>());
      }
    }

    if (payload.contains("impact_score") && payload["impact_score"].is_number()) {
      region.impactScore = payload["impact_score"].get<double>();
    }
  } catch (const std::exception& ex) {
    throwContractViolation(std::string("Impact query failed: ") + ex.what());
  } catch (...) {
    throwContractViolation("Impact query failed: unknown error.");
  }

  return region;
}

BranchDiffReport CognitiveKernelAPI::diffBranches(const std::string& branchA,
                                                  const std::string& branchB) {
  const std::filesystem::path projectRoot = std::filesystem::current_path();
  intelligence::BranchStore store;
  intelligence::BranchPersistence persistence(projectRoot / ".ultra");
  if (!persistence.load(store)) {
    throwContractViolation("Branch diff failed: unable to load branch store.");
  }

  const std::optional<intelligence::Branch> branchRefA =
      store.branchSnapshot(branchA);
  const std::optional<intelligence::Branch> branchRefB =
      store.branchSnapshot(branchB);
  if (!branchRefA.has_value() || !branchRefB.has_value()) {
    throwContractViolation("Branch diff failed: branch reference not found.");
  }

  std::uint64_t snapshotAId = 0ULL;
  std::uint64_t snapshotBId = 0ULL;
  if (!parseSnapshotId(branchRefA->memorySnapshotId, snapshotAId) ||
      !parseSnapshotId(branchRefB->memorySnapshotId, snapshotBId)) {
    throwContractViolation("Branch diff failed: invalid branch snapshot id.");
  }

  memory::SnapshotPersistence snapshotPersistence(projectRoot / ".ultra" / "memory");
  memory::StateGraph graphA;
  memory::StateGraph graphB;
  if (!snapshotPersistence.loadGraph(snapshotAId, graphA) ||
      !snapshotPersistence.loadGraph(snapshotBId, graphB)) {
    throwContractViolation("Branch diff failed: snapshot graph is unavailable.");
  }

  const memory::StateSnapshot snapshotA = graphA.snapshot(snapshotAId);
  const memory::StateSnapshot snapshotB = graphB.snapshot(snapshotBId);
  const diff::semantic::BranchDiffReport report =
      diff::DiffEngine::diffBranches(snapshotA, snapshotB);

  BranchDiffReport out;
  out.symbols.reserve(report.symbols.size());
  for (const diff::semantic::SymbolDiff& symbol : report.symbols) {
    out.symbols.push_back({symbol.id, mapDiffType(symbol.type)});
  }

  out.signatures.reserve(report.signatures.size());
  for (const diff::semantic::SignatureDiff& signature : report.signatures) {
    out.signatures.push_back(
        {signature.id, mapSignatureChange(signature.change)});
  }

  out.dependencies.reserve(report.dependencies.size());
  for (const diff::semantic::DependencyDiff& dependency : report.dependencies) {
    out.dependencies.push_back(
        {dependency.from, dependency.to, mapDiffType(dependency.type)});
  }

  std::sort(out.symbols.begin(), out.symbols.end(), lessSymbolDiff);
  std::sort(out.signatures.begin(), out.signatures.end(), lessSignatureDiff);
  std::sort(out.dependencies.begin(), out.dependencies.end(), lessDependencyDiff);

  out.overallRisk = mapRiskLevel(report.overallRisk);
  out.impactScore = report.impactScore;
  return out;
}

std::vector<runtime::intent::PlanScore> CognitiveKernelAPI::evaluateIntent(
    const runtime::CognitiveState& state,
    Intent intent,
    const runtime::TokenBudget budget) {
  validateCognitiveState(state);
  if (budget == 0U) {
    throwContractViolation("Intent evaluation failed: token budget is zero.");
  }

  const runtime::TokenBudget boundedBudget =
      std::min<runtime::TokenBudget>(kTokenBudgetCeiling, budget);
  runtime::CognitiveState evaluationState = state;
  evaluationState.budget = boundedBudget;

  runtime::intent::Intent normalizedIntent =
      runtime::intent::normalizeIntent(intent, boundedBudget);
  normalizedIntent.constraints.tokenBudget =
      std::min<std::size_t>(normalizedIntent.constraints.tokenBudget,
                            boundedBudget);
  normalizedIntent.constraints.tokenBudget =
      std::max<std::size_t>(1U, normalizedIntent.constraints.tokenBudget);

  try {
    runtime::intent::IntentEvaluator evaluator;
    const std::vector<runtime::intent::Strategy> strategies =
        evaluator.generateStrategies(normalizedIntent, evaluationState);
    const std::vector<runtime::intent::PlanScore> ranked =
        evaluator.evaluateStrategies(strategies, evaluationState);

    memory::CognitiveMemoryManager memoryManager(std::filesystem::current_path());
    memoryManager.bindToSnapshot(&evaluationState.snapshot);
    for (const runtime::intent::PlanScore& plan : ranked) {
      memoryManager.recordIntentEvaluation(
          "intent_rank:" + std::to_string(plan.rank) + ":" + plan.strategy.name,
          evaluationState.snapshot, plan.score, plan.strategy.risk.value,
          plan.determinismScore, "ranked_plan");
    }
    return ranked;
  } catch (const std::exception& ex) {
    throwContractViolation(std::string("Intent evaluation failed: ") + ex.what());
  } catch (...) {
    throwContractViolation("Intent evaluation failed: unknown error.");
  }
}

std::vector<runtime::governance::GovernedStrategyResult>
CognitiveKernelAPI::evaluateAndGovernIntent(const runtime::CognitiveState& state,
                                            Intent intent,
                                            Policy policy,
                                            const runtime::TokenBudget budget) {
  validateCognitiveState(state);
  if (budget == 0U) {
    throwContractViolation("Governed intent evaluation failed: token budget is zero.");
  }

  const runtime::TokenBudget boundedBudget =
      std::min<runtime::TokenBudget>(kTokenBudgetCeiling, budget);
  runtime::CognitiveState evaluationState = state;
  evaluationState.budget = boundedBudget;
  const Policy normalizedPolicy =
      runtime::governance::normalizePolicy(policy, static_cast<int>(boundedBudget));

  try {
    const std::vector<runtime::intent::PlanScore> rankedPlans =
        evaluateIntent(evaluationState, std::move(intent), boundedBudget);
    memory::CognitiveMemoryManager memoryManager(std::filesystem::current_path());
    memoryManager.bindToSnapshot(&evaluationState.snapshot);
    runtime::governance::GovernanceEngine engine(&memoryManager);

    std::vector<runtime::governance::GovernedStrategyResult> governed;
    governed.reserve(rankedPlans.size());
    for (const runtime::intent::PlanScore& plan : rankedPlans) {
      runtime::governance::GovernedStrategyResult result;
      result.plan = plan;
      result.governance = engine.evaluate(plan.strategy, normalizedPolicy,
                                          evaluationState);
      governed.push_back(std::move(result));
    }
    return governed;
  } catch (const std::exception& ex) {
    throwContractViolation(std::string("Governed intent evaluation failed: ") +
                           ex.what());
  } catch (...) {
    throwContractViolation("Governed intent evaluation failed: unknown error.");
  }
}

std::string CognitiveKernelAPI::compressContext(
    const runtime::CognitiveState& state) {
  validateCognitiveState(state);
  Query query;
  query.kind = QueryKind::Auto;
  query.target.clear();
  query.impactDepth = 1U;
  return getMinimalContext(state, query);
}

double CognitiveKernelAPI::estimateTokenSavings() {
  return metrics::PerformanceMetrics::averageTokenSavingsRatio();
}

HotSliceStats CognitiveKernelAPI::getHotSliceStats() {
  HotSliceStats stats;
  stats.currentSize = 0U;
  stats.capacity = memory::HotSlice::kMaxHotSliceEntries;
  return stats;
}

nlohmann::json CognitiveKernelAPI::queryTarget(
    const runtime::CognitiveState& state,
    const std::string& target,
    const std::filesystem::path& projectRoot) {
  validateCognitiveState(state);
  runtime::QueryEngine queryEngine(projectRoot);
  nlohmann::json payload = queryEngine.queryTarget(state.snapshot, target);
  if (payload.value("kind", "") == "file") {
    const std::string indexedPath = payload.value("path", "");
    payload["size"] = queryEngine.fileSizeForIndexedPath(indexedPath);
  }
  validateDeterministicOutput(payload);
  return payload;
}

nlohmann::json CognitiveKernelAPI::queryImpact(
    const runtime::CognitiveState& state,
    const std::string& target,
    const std::filesystem::path& projectRoot) {
  validateCognitiveState(state);
  runtime::QueryEngine queryEngine(projectRoot);
  nlohmann::json payload = queryEngine.queryImpact(state, target);
  validateDeterministicOutput(payload);
  return payload;
}

bool CognitiveKernelAPI::readSource(const runtime::CognitiveState& state,
                                    const std::string& fileTarget,
                                    const std::filesystem::path& projectRoot,
                                    nlohmann::json& payloadOut,
                                    std::string& error) {
  validateCognitiveState(state);
  runtime::QueryEngine queryEngine(projectRoot);
  const std::string indexedPath =
      queryEngine.resolveIndexedFilePath(state.snapshot, fileTarget);
  if (indexedPath.empty()) {
    error = "File is not indexed: " + fileTarget;
    payloadOut = nlohmann::json::object();
    return false;
  }
  const bool ok =
      queryEngine.readSourceByIndexedPath(indexedPath, payloadOut, error);
  if (ok) {
    validateDeterministicOutput(payloadOut);
  }
  return ok;
}

const char* toString(const DiffType type) {
  switch (type) {
    case DiffType::Added:
      return "Added";
    case DiffType::Removed:
      return "Removed";
    case DiffType::Renamed:
      return "Renamed";
    case DiffType::Moved:
      return "Moved";
    case DiffType::Modified:
      return "Modified";
  }
  return "Modified";
}

const char* toString(const SignatureChange change) {
  switch (change) {
    case SignatureChange::Signature:
      return "SIGNATURE";
    case SignatureChange::Visibility:
      return "VISIBILITY";
    case SignatureChange::Relocation:
      return "RELOCATION";
    case SignatureChange::Rename:
      return "RENAME";
  }
  return "SIGNATURE";
}

const char* toString(const RiskLevel level) {
  switch (level) {
    case RiskLevel::LOW:
      return "LOW";
    case RiskLevel::MEDIUM:
      return "MEDIUM";
    case RiskLevel::HIGH:
      return "HIGH";
  }
  return "LOW";
}

}  // namespace ultra::api
