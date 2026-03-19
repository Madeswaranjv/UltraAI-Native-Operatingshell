#include "failure_recovery.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace ultra::runtime::cognitive {

namespace {

std::string normalizeMessage(const std::string_view message) {
  std::string normalized;
  normalized.reserve(message.size());

  for (const char ch : message) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (std::isalnum(uch) != 0) {
      normalized.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    } else {
      normalized.push_back(' ');
    }
  }

  return normalized;
}

bool containsAny(const std::string& text,
                 const std::initializer_list<std::string_view> terms) {
  return std::any_of(terms.begin(), terms.end(),
                     [&text](const std::string_view term) {
                       return text.find(term) != std::string::npos;
                     });
}

}  // namespace

const char* toString(const FailureClass failureClass) noexcept {
  switch (failureClass) {
    case FailureClass::TRANSIENT_ERROR:
      return "TRANSIENT_ERROR";
    case FailureClass::VALIDATION_ERROR:
      return "VALIDATION_ERROR";
    case FailureClass::DEPENDENCY_ERROR:
      return "DEPENDENCY_ERROR";
    case FailureClass::CRITICAL_ERROR:
      return "CRITICAL_ERROR";
  }
  return "CRITICAL_ERROR";
}

const char* toString(const RecoveryAction action) noexcept {
  switch (action) {
    case RecoveryAction::RETRY_TASK:
      return "RETRY_TASK";
    case RecoveryAction::SKIP_TASK:
      return "SKIP_TASK";
    case RecoveryAction::REPLAN_REQUIRED:
      return "REPLAN_REQUIRED";
    case RecoveryAction::ABORT_LOOP:
      return "ABORT_LOOP";
  }
  return "ABORT_LOOP";
}

FailureClass FailureRecoveryEngine::classify(const FailureContext& ctx) const {
  const std::string message = normalizeMessage(ctx.execution_result.message);

  if (ctx.execution_result.ok && !ctx.execution_result.rolledBack) {
    return FailureClass::TRANSIENT_ERROR;
  }

  if (containsAny(message,
                  {"fatal", "critical", "assert", "corrupt", "invariant",
                   "out of memory", "unknown error"})) {
    return FailureClass::CRITICAL_ERROR;
  }

  if (containsAny(message,
                  {"dependency", "dependencies", "branch diff",
                   "comparison snapshot", "blocked by"})) {
    return FailureClass::DEPENDENCY_ERROR;
  }

  if (ctx.execution_result.rolledBack ||
      containsAny(message,
                  {"validation", "invalid", "does not match", "missing",
                   "empty", "requires", "rejected"})) {
    return FailureClass::VALIDATION_ERROR;
  }

  if (containsAny(message,
                  {"timeout", "temporarily", "busy", "rate limit",
                   "unavailable", "retry"})) {
    return FailureClass::TRANSIENT_ERROR;
  }

  return FailureClass::TRANSIENT_ERROR;
}

RecoveryAction FailureRecoveryEngine::decide(const FailureContext& ctx) const {
  const FailureClass failureClass = classify(ctx);

  if (failureClass == FailureClass::CRITICAL_ERROR) {
    return RecoveryAction::ABORT_LOOP;
  }

  if (failureClass == FailureClass::DEPENDENCY_ERROR) {
    return RecoveryAction::REPLAN_REQUIRED;
  }

  if (failureClass == FailureClass::VALIDATION_ERROR) {
    if (ctx.execution_result.rolledBack) {
      return RecoveryAction::REPLAN_REQUIRED;
    }
    return RecoveryAction::SKIP_TASK;
  }

  if (ctx.retry_count < ctx.retry_limit) {
    return RecoveryAction::RETRY_TASK;
  }

  if (ctx.dependency_state.has_value() &&
      ctx.dependency_state->failedTaskCount > 1U) {
    return RecoveryAction::REPLAN_REQUIRED;
  }

  return RecoveryAction::REPLAN_REQUIRED;
}

}  // namespace ultra::runtime::cognitive

