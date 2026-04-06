#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ultra::runtime::contracts {

enum class LayerId : std::uint8_t {
  L4_GRAPH = 0U,
  L5_OVERLAY = 1U,
  L15_EXECUTION_KERNEL = 2U,
  L24_MEMORY = 3U,
};

enum class ViolationType : std::uint8_t {
  InvariantFailure = 0U,
  ImmutableMutation = 1U,
  OverlayBypass = 2U,
  MemoryPhaseViolation = 3U,
  ExecutionBypass = 4U,
};

enum class LoopPhase : std::uint8_t {
  UNSPECIFIED = 0U,
  INIT = 1U,
  PLAN = 2U,
  ARBITRATION = 3U,
  MICRO_PLAN = 4U,
  EXECUTE = 5U,
  PARTIAL_REPAIR = 6U,
  VERIFY = 7U,
  REFLECT = 8U,
  RE_ANCHOR = 9U,
  REPLAN = 10U,
  TERMINATE = 11U,
};

[[nodiscard]] const char* toString(LayerId layer) noexcept;
[[nodiscard]] const char* toString(ViolationType violation) noexcept;
[[nodiscard]] const char* toString(LoopPhase phase) noexcept;

struct LayerContract {
  LayerId layer{LayerId::L4_GRAPH};
  std::vector<std::string_view> allowedInputs;
  std::vector<std::string_view> allowedOutputs;
  std::vector<std::string_view> forbiddenActions;
  std::vector<std::string_view> invariants;
};

struct ContractViolationReport {
  LayerId layer{LayerId::L4_GRAPH};
  ViolationType violationType{ViolationType::InvariantFailure};
  std::string location;
  std::string detail;
  LoopPhase phase{LoopPhase::UNSPECIFIED};
};

class ContractViolationException final : public std::logic_error {
 public:
  explicit ContractViolationException(ContractViolationReport report);

  [[nodiscard]] const ContractViolationReport& report() const noexcept {
    return report_;
  }

 private:
  ContractViolationReport report_;
};

class ScopedLoopPhase final {
 public:
  explicit ScopedLoopPhase(LoopPhase phase) noexcept;
  ~ScopedLoopPhase();

  ScopedLoopPhase(const ScopedLoopPhase&) = delete;
  ScopedLoopPhase& operator=(const ScopedLoopPhase&) = delete;

 private:
  LoopPhase previous_{LoopPhase::UNSPECIFIED};
  bool hadPrevious_{false};
};

class ScopedTaskGraphAuthorization final {
 public:
  explicit ScopedTaskGraphAuthorization(std::string taskId);
  ~ScopedTaskGraphAuthorization();

  ScopedTaskGraphAuthorization(const ScopedTaskGraphAuthorization&) = delete;
  ScopedTaskGraphAuthorization& operator=(
      const ScopedTaskGraphAuthorization&) = delete;

 private:
  std::string previousTaskId_;
  bool hadPrevious_{false};
};

class ContractValidator final {
 public:
  [[nodiscard]] static const LayerContract& contract(LayerId layer) noexcept;
  [[nodiscard]] static LoopPhase currentPhase() noexcept;
  [[nodiscard]] static bool hasPhaseContext() noexcept;
  [[nodiscard]] static bool memoryReadAllowed(LoopPhase phase) noexcept;
  [[nodiscard]] static bool memoryWriteAllowed(LoopPhase phase) noexcept;

  static void assertMemoryRead(std::string_view location);
  static void assertMemoryWrite(std::string_view location);
  static void assertTaskGraphExecution(std::string_view taskId,
                                       std::string_view location);
  static void assertInvariant(bool condition,
                              LayerId layer,
                              ViolationType violationType,
                              std::string_view location,
                              std::string_view detail);
};

}  // namespace ultra::runtime::contracts
