#pragma once

#include <external/json.hpp>
#include "../types/Timestamp.h"
//E:\Projects\Ultra\src\memory\EpisodicMemory.h
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ultra::memory {

enum class EpisodicEventType : std::uint8_t {
  SnapshotBound = 0,
  IntentStart = 1,
  IntentEvaluation = 2,
  RiskEvaluation = 3,
  ExecutionSuccess = 4,
  ExecutionFailure = 5,
  Rollback = 6,
  MergeAttempt = 7,
  MergeSuccess = 8,
  MergeRejected = 9
};

struct EpisodicEvent {
  std::uint64_t sequence{0U};
  std::uint64_t version{0U};
  std::string branchId;
  EpisodicEventType type{EpisodicEventType::IntentStart};
  std::string subject;
  bool success{false};
  bool rolledBack{false};
  double riskScore{0.0};
  double confidenceScore{0.0};
  ultra::types::Timestamp timestamp;
  std::string message;
};

class EpisodicMemory {
 public:
  explicit EpisodicMemory(std::size_t retentionLimit = 4096U);

  void recordEvent(const EpisodicEvent& event);
  [[nodiscard]] std::vector<EpisodicEvent> getEventsForVersion(
      std::uint64_t version) const;
  void pruneOlderThan(std::uint64_t minVersionInclusive);

  [[nodiscard]] std::vector<EpisodicEvent> snapshot() const;
  [[nodiscard]] bool persistToFile(const std::filesystem::path& path) const;
  [[nodiscard]] bool loadFromFile(const std::filesystem::path& path);

 private:
  static constexpr std::uint32_t kSchemaVersion = 1U;

  static const char* toString(EpisodicEventType type);
  static EpisodicEventType fromString(const std::string& value);

  mutable std::shared_mutex mutex_;
  std::deque<EpisodicEvent> events_;
  std::size_t retentionLimit_{0U};
  std::uint64_t nextSequence_{1U};
};

}  // namespace ultra::memory
