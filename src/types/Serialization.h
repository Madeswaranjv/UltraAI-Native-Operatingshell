#pragma once

#include "Confidence.h"
#include "StructuredError.h"
#include "StructuredOutput.h"
#include "Timestamp.h"
#include "BranchId.h"
#include "external/json.hpp"

/// JSON serialization/deserialization for all Ultra core types.
///
/// Uses nlohmann::json ADL (Argument-Dependent Lookup) overloads so that
/// types can be used directly with nlohmann::json:
///
///   ultra::types::Timestamp ts = ultra::types::Timestamp::now();
///   nlohmann::json j = ts;              // to_json
///   auto ts2 = j.get<ultra::types::Timestamp>(); // from_json

namespace ultra::types {

// ───────────────────── Timestamp ─────────────────────

inline void to_json(nlohmann::json& j, const Timestamp& ts) {
  j = ts.toIso8601();
}

inline void from_json(const nlohmann::json& j, Timestamp& ts) {
  ts = Timestamp::fromIso8601(j.get<std::string>());
}

// ───────────────────── BranchId ──────────────────────

inline void to_json(nlohmann::json& j, const BranchId& id) {
  j = id.toString();
}

inline void from_json(const nlohmann::json& j, BranchId& id) {
  id = BranchId::fromString(j.get<std::string>());
}

// ───────────────────── Confidence ────────────────────

inline void to_json(nlohmann::json& j, const Confidence& c) {
  j = nlohmann::json{
      {"stability_score", c.stabilityScore},
      {"risk_adjusted_confidence", c.riskAdjustedConfidence},
      {"decision_reliability_index", c.decisionReliabilityIndex},
      {"overall", c.overall()}};
}

inline void from_json(const nlohmann::json& j, Confidence& c) {
  j.at("stability_score").get_to(c.stabilityScore);
  j.at("risk_adjusted_confidence").get_to(c.riskAdjustedConfidence);
  j.at("decision_reliability_index").get_to(c.decisionReliabilityIndex);
}

// ───────────────────── StructuredError ────────────────

inline void to_json(nlohmann::json& j, const StructuredError& e) {
  j = nlohmann::json{
      {"error_type", e.errorType},
      {"message", e.message},
      {"severity", e.severity},
      {"suggested_action", e.suggestedAction}};
  if (!e.symbol.empty()) {
    j["symbol"] = e.symbol;
  }
  if (!e.sourceFile.empty()) {
    j["source_file"] = e.sourceFile;
    if (e.sourceLine > 0) {
      j["source_line"] = e.sourceLine;
    }
  }
  if (!e.context.empty()) {
    j["context"] = e.context;
  }
  j["timestamp"] = e.timestamp;
}

inline void from_json(const nlohmann::json& j, StructuredError& e) {
  j.at("error_type").get_to(e.errorType);
  j.at("message").get_to(e.message);
  j.at("severity").get_to(e.severity);
  j.at("suggested_action").get_to(e.suggestedAction);
  if (j.contains("symbol")) j.at("symbol").get_to(e.symbol);
  if (j.contains("source_file")) j.at("source_file").get_to(e.sourceFile);
  if (j.contains("source_line")) j.at("source_line").get_to(e.sourceLine);
  if (j.contains("context")) j.at("context").get_to(e.context);
  if (j.contains("timestamp")) j.at("timestamp").get_to(e.timestamp);
}

// ───────────────────── StructuredOutput ───────────────

inline void to_json(nlohmann::json& j, const StructuredOutput& o) {
  j = nlohmann::json{
      {"status", outputStatusToString(o.status)},
      {"command", o.command},
      {"data", o.data},
      {"errors", nlohmann::json::array()},
      {"metadata", o.metadata},
      {"timestamp", o.timestamp}};
  for (const auto& err : o.errors) {
    nlohmann::json ej;
    to_json(ej, err);
    j["errors"].push_back(ej);
  }
}

inline void from_json(const nlohmann::json& j, StructuredOutput& o) {
  std::string statusStr;
  j.at("status").get_to(statusStr);
  if (statusStr == "success")
    o.status = OutputStatus::Success;
  else if (statusStr == "failure")
    o.status = OutputStatus::Failure;
  else if (statusStr == "partial")
    o.status = OutputStatus::Partial;
  else if (statusStr == "skipped")
    o.status = OutputStatus::Skipped;

  j.at("command").get_to(o.command);
  o.data = j.at("data");
  o.metadata = j.at("metadata");
  if (j.contains("timestamp")) j.at("timestamp").get_to(o.timestamp);
  if (j.contains("errors")) {
    for (const auto& ej : j.at("errors")) {
      StructuredError err;
      from_json(ej, err);
      o.errors.push_back(err);
    }
  }
}

}  // namespace ultra::types
