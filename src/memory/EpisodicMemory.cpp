#include "EpisodicMemory.h"
//E:\Projects\Ultra\src\memory\EpisodicMemory.cpp
#include <algorithm>
#include <fstream>
#include <limits>
#include <mutex>
#include <vector>

namespace ultra::memory {

namespace {

bool lessEvent(const EpisodicEvent& left, const EpisodicEvent& right) {
  if (left.sequence != right.sequence) {
    return left.sequence < right.sequence;
  }
  if (left.version != right.version) {
    return left.version < right.version;
  }
  if (left.branchId != right.branchId) {
    return left.branchId < right.branchId;
  }
  return left.subject < right.subject;
}

std::int64_t deterministicTimestampMs(const std::uint64_t version,
                                      const std::uint64_t sequence) {
  constexpr std::int64_t kMaxMs = std::numeric_limits<std::int64_t>::max();
  constexpr std::uint64_t kStride = 1000ULL;
  if (version > static_cast<std::uint64_t>(kMaxMs) / kStride) {
    return kMaxMs;
  }
  const std::uint64_t base = version * kStride;
  const std::uint64_t offset = sequence % kStride;
  const std::uint64_t value = base + offset;
  if (value > static_cast<std::uint64_t>(kMaxMs)) {
    return kMaxMs;
  }
  return static_cast<std::int64_t>(value);
}

}  // namespace

EpisodicMemory::EpisodicMemory(const std::size_t retentionLimit)
    : retentionLimit_(std::max<std::size_t>(1U, retentionLimit)) {}

void EpisodicMemory::recordEvent(const EpisodicEvent& event) {
  std::unique_lock lock(mutex_);
  EpisodicEvent stored = event;
  if (stored.sequence == 0U) {
    stored.sequence = nextSequence_;
  }
  nextSequence_ = std::max(nextSequence_, stored.sequence + 1U);
  if (stored.timestamp.epochMs() == 0LL) {
    stored.timestamp = ultra::types::Timestamp::fromEpochMs(
        deterministicTimestampMs(stored.version, stored.sequence));
  }
  events_.push_back(std::move(stored));
  while (events_.size() > retentionLimit_) {
    events_.pop_front();
  }
}

std::vector<EpisodicEvent> EpisodicMemory::getEventsForVersion(
    const std::uint64_t version) const {
  std::shared_lock lock(mutex_);
  std::vector<EpisodicEvent> out;
  for (const EpisodicEvent& event : events_) {
    if (event.version == version) {
      out.push_back(event);
    }
  }
  return out;
}

void EpisodicMemory::pruneOlderThan(const std::uint64_t minVersionInclusive) {
  std::unique_lock lock(mutex_);
  while (!events_.empty() && events_.front().version < minVersionInclusive) {
    events_.pop_front();
  }
}

std::vector<EpisodicEvent> EpisodicMemory::snapshot() const {
  std::shared_lock lock(mutex_);
  return std::vector<EpisodicEvent>(events_.begin(), events_.end());
}

bool EpisodicMemory::persistToFile(const std::filesystem::path& path) const {
  std::vector<EpisodicEvent> copy;
  {
    std::shared_lock lock(mutex_);
    copy.assign(events_.begin(), events_.end());
  }
  std::sort(copy.begin(), copy.end(), lessEvent);

  nlohmann::ordered_json payload;
  payload["schema_version"] = kSchemaVersion;
  payload["events"] = nlohmann::ordered_json::array();
  for (const EpisodicEvent& event : copy) {
    nlohmann::ordered_json item;
    item["sequence"] = event.sequence;
    item["version"] = event.version;
    item["branch_id"] = event.branchId;
    item["type"] = toString(event.type);
    item["subject"] = event.subject;
    item["success"] = event.success;
    item["rolled_back"] = event.rolledBack;
    item["risk_score"] = event.riskScore;
    item["confidence_score"] = event.confidenceScore;
    item["timestamp_ms"] = event.timestamp.epochMs();
    item["message"] = event.message;
    payload["events"].push_back(std::move(item));
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << payload.dump(2);
  return static_cast<bool>(out);
}

bool EpisodicMemory::loadFromFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }

  nlohmann::json payload;
  try {
    in >> payload;
  } catch (...) {
    return false;
  }
  if (!payload.is_object()) {
    return false;
  }
  if (payload.value("schema_version", 0U) != kSchemaVersion) {
    return false;
  }
  if (!payload.contains("events") || !payload["events"].is_array()) {
    return false;
  }

  std::vector<EpisodicEvent> loaded;
  loaded.reserve(payload["events"].size());
  for (const nlohmann::json& item : payload["events"]) {
    if (!item.is_object()) {
      continue;
    }
    EpisodicEvent event;
    event.sequence = item.value("sequence", 0U);
    event.version = item.value("version", 0U);
    event.branchId = item.value("branch_id", std::string{});
    event.type = fromString(item.value("type", std::string{}));
    event.subject = item.value("subject", std::string{});
    event.success = item.value("success", false);
    event.rolledBack = item.value("rolled_back", false);
    event.riskScore = item.value("risk_score", 0.0);
    event.confidenceScore = item.value("confidence_score", 0.0);
    event.timestamp = ultra::types::Timestamp::fromEpochMs(
        item.value("timestamp_ms", 0LL));
    event.message = item.value("message", std::string{});
    loaded.push_back(std::move(event));
  }

  std::sort(loaded.begin(), loaded.end(), lessEvent);

  std::unique_lock lock(mutex_);
  events_.clear();
  for (const EpisodicEvent& event : loaded) {
    events_.push_back(event);
  }
  while (events_.size() > retentionLimit_) {
    events_.pop_front();
  }

  nextSequence_ = 1U;
  for (const EpisodicEvent& event : events_) {
    nextSequence_ = std::max(nextSequence_, event.sequence + 1U);
  }
  return true;
}

const char* EpisodicMemory::toString(const EpisodicEventType type) {
  switch (type) {
    case EpisodicEventType::SnapshotBound:
      return "snapshot_bound";
    case EpisodicEventType::IntentStart:
      return "intent_start";
    case EpisodicEventType::IntentEvaluation:
      return "intent_evaluation";
    case EpisodicEventType::RiskEvaluation:
      return "risk_evaluation";
    case EpisodicEventType::ExecutionSuccess:
      return "execution_success";
    case EpisodicEventType::ExecutionFailure:
      return "execution_failure";
    case EpisodicEventType::Rollback:
      return "rollback";
    case EpisodicEventType::MergeAttempt:
      return "merge_attempt";
    case EpisodicEventType::MergeSuccess:
      return "merge_success";
    case EpisodicEventType::MergeRejected:
      return "merge_rejected";
  }
  return "intent_start";
}

EpisodicEventType EpisodicMemory::fromString(const std::string& value) {
  if (value == "snapshot_bound") {
    return EpisodicEventType::SnapshotBound;
  }
  if (value == "intent_start") {
    return EpisodicEventType::IntentStart;
  }
  if (value == "intent_evaluation") {
    return EpisodicEventType::IntentEvaluation;
  }
  if (value == "risk_evaluation") {
    return EpisodicEventType::RiskEvaluation;
  }
  if (value == "execution_success") {
    return EpisodicEventType::ExecutionSuccess;
  }
  if (value == "execution_failure") {
    return EpisodicEventType::ExecutionFailure;
  }
  if (value == "rollback") {
    return EpisodicEventType::Rollback;
  }
  if (value == "merge_attempt") {
    return EpisodicEventType::MergeAttempt;
  }
  if (value == "merge_success") {
    return EpisodicEventType::MergeSuccess;
  }
  if (value == "merge_rejected") {
    return EpisodicEventType::MergeRejected;
  }
  return EpisodicEventType::IntentStart;
}

}  // namespace ultra::memory
