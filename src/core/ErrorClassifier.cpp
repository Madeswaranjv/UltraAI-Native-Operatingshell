#include "ErrorClassifier.h"

#include <algorithm>
#include <cctype>

namespace ultra::core {

namespace {

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

ultra::types::StructuredError ErrorClassifier::classify(
    const std::string& rawMessage) {
  return classify(rawMessage, "");
}

ultra::types::StructuredError ErrorClassifier::classify(
    const std::string& rawMessage,
    const std::string& category) {
  ultra::types::StructuredError err;
  err.message = rawMessage;
  err.timestamp = ultra::types::Timestamp::now();

  const std::string lower = toLower(rawMessage);

  // ── Linker errors ──
  if (contains(lower, "undefined reference") ||
      contains(lower, "unresolved external") ||
      contains(lower, "linker error") ||
      contains(lower, "lnk2019") || contains(lower, "lnk2001") ||
      contains(lower, "lnk1120")) {
    err.errorType = "LinkerError";
    err.severity = 0.85;
    err.suggestedAction = "rebuild_dependency_graph";

    // Try to extract the symbol name.
    auto pos = lower.find("undefined reference to");
    if (pos != std::string::npos) {
      auto start = rawMessage.find('`', pos);
      auto end = rawMessage.find('\'', start + 1);
      if (start != std::string::npos && end != std::string::npos) {
        err.symbol = rawMessage.substr(start + 1, end - start - 1);
      }
    }
    return err;
  }

  // ── Syntax errors ──
  if (contains(lower, "syntax error") ||
      contains(lower, "expected ';'") || contains(lower, "expected '}'") ||
      contains(lower, "unexpected token") ||
      contains(lower, "c2059") || contains(lower, "c2143")) {
    err.errorType = "SyntaxError";
    err.severity = 0.7;
    err.suggestedAction = "check_syntax";
    return err;
  }

  // ── Type errors ──
  if (contains(lower, "cannot convert") ||
      contains(lower, "no matching function") ||
      contains(lower, "type mismatch") ||
      contains(lower, "c2664") || contains(lower, "c2440")) {
    err.errorType = "TypeError";
    err.severity = 0.7;
    err.suggestedAction = "check_types";
    return err;
  }

  // ── Include / dependency errors ──
  if (contains(lower, "no such file or directory") ||
      contains(lower, "cannot open include file") ||
      contains(lower, "file not found") ||
      contains(lower, "c1083")) {
    err.errorType = "IncludeError";
    err.severity = 0.6;
    err.suggestedAction = "check_include_paths";
    return err;
  }

  // ── File not found ──
  if (contains(lower, "invalid path") ||
      contains(lower, "path not found") ||
      contains(lower, "does not exist")) {
    err.errorType = "FileNotFound";
    err.severity = 0.5;
    err.suggestedAction = "check_path";
    return err;
  }

  // ── Permission errors ──
  if (contains(lower, "permission denied") ||
      contains(lower, "access denied") ||
      contains(lower, "access is denied")) {
    err.errorType = "PermissionDenied";
    err.severity = 0.6;
    err.suggestedAction = "check_permissions";
    return err;
  }

  // ── Config errors ──
  if (contains(lower, "config") ||
      contains(lower, "ultra.json") || contains(lower, "ultra.config")) {
    err.errorType = "ConfigError";
    err.severity = 0.5;
    err.suggestedAction = "check_config";
    return err;
  }

  // ── Command validation ──
  if (contains(lower, "requires exactly") ||
      contains(lower, "requires a") ||
      contains(lower, "does not accept") ||
      contains(lower, "unknown command")) {
    err.errorType = "CommandError";
    err.severity = 0.3;
    err.suggestedAction = "check_usage";
    return err;
  }

  // ── Default/unclassified ──
  err.errorType = category.empty() ? "GenericError" : category;
  err.severity = 0.5;
  err.suggestedAction = "investigate";
  return err;
}

ultra::types::StructuredError ErrorClassifier::classifyBuildError(
    const std::string& rawOutput) {
  return classify(rawOutput, "BuildError");
}

}  // namespace ultra::core
