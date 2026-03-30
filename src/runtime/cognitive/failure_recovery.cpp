#include "failure_recovery.h"

#include <algorithm>
#include <cctype>
#include <iostream>
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
  if (ctx.execution_result.ok && !ctx.execution_result.rolledBack) {
    std::cerr << "[FailureRecovery] Successful execution result reached recovery path."
              << " task=" << ctx.task_id
              << " retry=" << ctx.retry_count << "/" << ctx.retry_limit
              << " -> REPLAN_REQUIRED\n";
    return RecoveryAction::REPLAN_REQUIRED;
  }

  const FailureClass failureClass = classify(ctx);
  RecoveryAction action = RecoveryAction::REPLAN_REQUIRED;

  if (failureClass == FailureClass::CRITICAL_ERROR) {
    action = RecoveryAction::ABORT_LOOP;
  } else if (failureClass == FailureClass::DEPENDENCY_ERROR) {
    action = RecoveryAction::REPLAN_REQUIRED;
  } else if (failureClass == FailureClass::VALIDATION_ERROR) {
    action = ctx.execution_result.rolledBack ? RecoveryAction::REPLAN_REQUIRED
                                             : RecoveryAction::SKIP_TASK;
  } else if (ctx.retry_count < ctx.retry_limit) {
    action = RecoveryAction::RETRY_TASK;
  } else if (ctx.dependency_state.has_value() &&
             ctx.dependency_state->failedTaskCount > 1U) {
    action = RecoveryAction::REPLAN_REQUIRED;
  }

  std::cerr << "[FailureRecovery] task=" << ctx.task_id
            << " class=" << toString(failureClass)
            << " retry=" << ctx.retry_count << "/" << ctx.retry_limit
            << " action=" << toString(action)
            << " message=" << ctx.execution_result.message << "\n";
  return action;
}

}  // namespace ultra::runtime::cognitive

