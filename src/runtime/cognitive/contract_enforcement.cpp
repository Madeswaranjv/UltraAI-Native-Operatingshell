#include "contract_enforcement.h"

#include <utility>

namespace ultra::runtime::contracts {

namespace {

thread_local bool g_hasPhaseContext = false;
thread_local LoopPhase g_currentPhase = LoopPhase::UNSPECIFIED;
thread_local bool g_hasAuthorizedTask = false;
thread_local std::string g_authorizedTaskId;

[[nodiscard]] std::string buildViolationMessage(
    const ContractViolationReport& report) {
  std::string message = std::string(toString(report.layer));
  message += " contract violation (";
  message += toString(report.violationType);
  message += ") at ";
  message += report.location.empty() ? std::string("<unknown>") : report.location;
  if (report.phase != LoopPhase::UNSPECIFIED) {
    message += " [phase=";
    message += toString(report.phase);
    message += "]";
  }
  message += ": ";
  message += report.detail.empty() ? std::string("invariant failed")
                                   : report.detail;
  return message;
}

}  // namespace

const char* toString(const LayerId layer) noexcept {
  switch (layer) {
    case LayerId::L4_GRAPH:
      return "L4 Graph";
    case LayerId::L5_OVERLAY:
      return "L5 Overlay";
    case LayerId::L15_EXECUTION_KERNEL:
      return "L15 Execution Kernel";
    case LayerId::L24_MEMORY:
      return "L24 Memory";
  }
  return "L4 Graph";
}

const char* toString(const ViolationType violation) noexcept {
  switch (violation) {
    case ViolationType::InvariantFailure:
      return "invariant_failure";
    case ViolationType::ImmutableMutation:
      return "immutable_mutation";
    case ViolationType::OverlayBypass:
      return "overlay_bypass";
    case ViolationType::MemoryPhaseViolation:
      return "memory_phase_violation";
    case ViolationType::ExecutionBypass:
      return "execution_bypass";
  }
  return "invariant_failure";
}

const char* toString(const LoopPhase phase) noexcept {
  switch (phase) {
    case LoopPhase::UNSPECIFIED:
      return "UNSPECIFIED";
    case LoopPhase::INIT:
      return "INIT";
    case LoopPhase::PLAN:
      return "PLAN";
    case LoopPhase::ARBITRATION:
      return "ARBITRATION";
    case LoopPhase::MICRO_PLAN:
      return "MICRO_PLAN";
    case LoopPhase::EXECUTE:
      return "EXECUTE";
    case LoopPhase::PARTIAL_REPAIR:
      return "PARTIAL_REPAIR";
    case LoopPhase::VERIFY:
      return "VERIFY";
    case LoopPhase::REFLECT:
      return "REFLECT";
    case LoopPhase::RE_ANCHOR:
      return "RE_ANCHOR";
    case LoopPhase::REPLAN:
      return "REPLAN";
    case LoopPhase::TERMINATE:
      return "TERMINATE";
  }
  return "UNSPECIFIED";
}

ContractViolationException::ContractViolationException(
    ContractViolationReport report)
    : std::logic_error(buildViolationMessage(report)),
      report_(std::move(report)) {}

ScopedLoopPhase::ScopedLoopPhase(const LoopPhase phase) noexcept
    : previous_(g_currentPhase), hadPrevious_(g_hasPhaseContext) {
  g_currentPhase = phase;
  g_hasPhaseContext = true;
}

ScopedLoopPhase::~ScopedLoopPhase() {
  if (hadPrevious_) {
    g_currentPhase = previous_;
    g_hasPhaseContext = true;
    return;
  }

  g_currentPhase = LoopPhase::UNSPECIFIED;
  g_hasPhaseContext = false;
}

ScopedTaskGraphAuthorization::ScopedTaskGraphAuthorization(std::string taskId)
    : previousTaskId_(g_authorizedTaskId), hadPrevious_(g_hasAuthorizedTask) {
  g_authorizedTaskId = std::move(taskId);
  g_hasAuthorizedTask = true;
}

ScopedTaskGraphAuthorization::~ScopedTaskGraphAuthorization() {
  if (hadPrevious_) {
    g_authorizedTaskId = previousTaskId_;
    g_hasAuthorizedTask = true;
    return;
  }

  g_authorizedTaskId.clear();
  g_hasAuthorizedTask = false;
}

const LayerContract& ContractValidator::contract(const LayerId layer) noexcept {
  static const LayerContract kGraphContract{
      LayerId::L4_GRAPH,
      {"StateGraphBuilder", "GraphSnapshot"},
      {"const StateGraph view", "deterministic hash"},
      {"mutation after snapshot creation"},
      {"GraphSnapshot is immutable", "deterministic hash remains stable"},
  };
  static const LayerContract kOverlayContract{
      LayerId::L5_OVERLAY,
      {"GraphSnapshot", "WriteMutation"},
      {"KernelMutationOutcome", "overlay snapshot"},
      {"direct graph writes", "stale snapshot mutation"},
      {"all graph mutations go through overlay application"},
  };
  static const LayerContract kExecutionContract{
      LayerId::L15_EXECUTION_KERNEL,
      {"TaskGraph task payload", "pinned cognitive state"},
      {"runtime::Result"},
      {"planner bypass", "direct memory mutation"},
      {"execute only task-graph-approved work"},
  };
  static const LayerContract kMemoryContract{
      LayerId::L24_MEMORY,
      {"bounded query", "phase-scoped writes"},
      {"episodic/strategic matches", "bounded working-memory binding"},
      {"unphased writes", "execute-phase persistent writes"},
      {"persistent memory is phase-guarded and deterministic"},
  };

  switch (layer) {
    case LayerId::L4_GRAPH:
      return kGraphContract;
    case LayerId::L5_OVERLAY:
      return kOverlayContract;
    case LayerId::L15_EXECUTION_KERNEL:
      return kExecutionContract;
    case LayerId::L24_MEMORY:
      return kMemoryContract;
  }
  return kGraphContract;
}

LoopPhase ContractValidator::currentPhase() noexcept {
  return g_hasPhaseContext ? g_currentPhase : LoopPhase::UNSPECIFIED;
}

bool ContractValidator::hasPhaseContext() noexcept {
  return g_hasPhaseContext;
}

bool ContractValidator::memoryReadAllowed(const LoopPhase phase) noexcept {
  switch (phase) {
    case LoopPhase::PLAN:
    case LoopPhase::ARBITRATION:
    case LoopPhase::MICRO_PLAN:
    case LoopPhase::EXECUTE:
    case LoopPhase::PARTIAL_REPAIR:
    case LoopPhase::VERIFY:
    case LoopPhase::REFLECT:
    case LoopPhase::RE_ANCHOR:
    case LoopPhase::REPLAN:
    case LoopPhase::TERMINATE:
      return true;
    case LoopPhase::UNSPECIFIED:
    case LoopPhase::INIT:
      return false;
  }
  return false;
}

bool ContractValidator::memoryWriteAllowed(const LoopPhase phase) noexcept {
  switch (phase) {
    case LoopPhase::PLAN:
    case LoopPhase::VERIFY:
    case LoopPhase::REFLECT:
    case LoopPhase::REPLAN:
    case LoopPhase::TERMINATE:
      return true;
    case LoopPhase::UNSPECIFIED:
    case LoopPhase::INIT:
    case LoopPhase::ARBITRATION:
    case LoopPhase::MICRO_PLAN:
    case LoopPhase::EXECUTE:
    case LoopPhase::PARTIAL_REPAIR:
    case LoopPhase::RE_ANCHOR:
      return false;
  }
  return false;
}

void ContractValidator::assertMemoryRead(const std::string_view location) {
  if (!g_hasPhaseContext) {
    return;
  }
  if (memoryReadAllowed(g_currentPhase)) {
    return;
  }

  throw ContractViolationException({
      LayerId::L24_MEMORY,
      ViolationType::MemoryPhaseViolation,
      std::string(location),
      std::string("memory reads are forbidden in phase ") +
          toString(g_currentPhase),
      g_currentPhase,
  });
}

void ContractValidator::assertMemoryWrite(const std::string_view location) {
  if (!g_hasPhaseContext) {
    return;
  }
  if (memoryWriteAllowed(g_currentPhase)) {
    return;
  }

  throw ContractViolationException({
      LayerId::L24_MEMORY,
      ViolationType::MemoryPhaseViolation,
      std::string(location),
      std::string("persistent memory writes are forbidden in phase ") +
          toString(g_currentPhase),
      g_currentPhase,
  });
}

void ContractValidator::assertTaskGraphExecution(
    const std::string_view taskId,
    const std::string_view location) {
  (void)taskId;
  if (!g_hasAuthorizedTask) {
    throw ContractViolationException({
        LayerId::L15_EXECUTION_KERNEL,
        ViolationType::ExecutionBypass,
        std::string(location),
        "execution kernel requires task-graph authorization",
        currentPhase(),
    });
  }
}

void ContractValidator::assertInvariant(const bool condition,
                                        const LayerId layer,
                                        const ViolationType violationType,
                                        const std::string_view location,
                                        const std::string_view detail) {
  if (condition) {
    return;
  }

  throw ContractViolationException({
      layer,
      violationType,
      std::string(location),
      std::string(detail),
      currentPhase(),
  });
}

}  // namespace ultra::runtime::contracts
