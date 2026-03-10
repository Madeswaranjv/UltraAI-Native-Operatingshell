// =====================================================
// Structured Error Tests
// Tests for StructuredError: machine-readable error structures
// =====================================================

#include <gtest/gtest.h>
#include "types/StructuredError.h"
#include "types/Timestamp.h"
#include "types/Serialization.h"
#include <external/json.hpp>

namespace ultra::types {

class StructuredErrorTest : public ::testing::Test {
 protected:
  StructuredError createError() {
    StructuredError err;
    err.errorType = "LinkerError";
    err.symbol = "undefined_function";
    err.severity = 0.83;
    err.suggestedAction = "rebuild_deps";
    err.sourceFile = "main.cpp";
    err.sourceLine = 42;
    err.message = "Undefined reference to 'initialize'";
    err.timestamp = Timestamp::now();
    return err;
  }
};

// ===== Basic Error Construction =====

TEST_F(StructuredErrorTest, DefaultConstruction) {
  StructuredError err;
  EXPECT_TRUE(err.errorType.empty());
  EXPECT_TRUE(err.symbol.empty());
  EXPECT_EQ(err.severity, 0.5);
  EXPECT_TRUE(err.suggestedAction.empty());
  EXPECT_TRUE(err.sourceFile.empty());
  EXPECT_EQ(err.sourceLine, 0);
  EXPECT_TRUE(err.context.empty());
  EXPECT_TRUE(err.message.empty());
}

TEST_F(StructuredErrorTest, ErrorCodeAssignment) {
  std::vector<std::string> types = {
      "LinkerError", "SyntaxError", "ConfigError", "FileNotFound", "PermissionDenied"};

  for (const auto& type : types) {
    StructuredError err = createError();
    err.errorType = type;
    EXPECT_EQ(err.errorType, type);
  }
}

TEST_F(StructuredErrorTest, ErrorMessagePreservation) {
  StructuredError err = createError();
  std::string msg = "Undefined reference to 'initialize'";
  err.message = msg;
  EXPECT_EQ(err.message, msg);
}

TEST_F(StructuredErrorTest, SymbolPreservation) {
  StructuredError err = createError();
  EXPECT_EQ(err.symbol, "undefined_function");

  err.symbol = "my_function";
  EXPECT_EQ(err.symbol, "my_function");
}

// ===== Severity Scoring =====

TEST_F(StructuredErrorTest, SeverityLow) {
  StructuredError err = createError();
  err.severity = 0.0;  // Info level
  EXPECT_EQ(err.severity, 0.0);
}

TEST_F(StructuredErrorTest, SeverityMid) {
  StructuredError err = createError();
  err.severity = 0.5;  // Warning level
  EXPECT_EQ(err.severity, 0.5);
}

TEST_F(StructuredErrorTest, SeverityHigh) {
  StructuredError err = createError();
  err.severity = 1.0;  // Fatal level
  EXPECT_EQ(err.severity, 1.0);
}

TEST_F(StructuredErrorTest, SeverityPrecision) {
  StructuredError err = createError();
  err.severity = 0.75;
  EXPECT_NEAR(err.severity, 0.75, 0.0001);
}

// ===== Suggested Action Field =====

TEST_F(StructuredErrorTest, SuggestedActionAssignment) {
  std::vector<std::string> actions = {
      "rebuild_deps", "check_path", "update_config", "verify_permissions", "retry"};

  for (const auto& action : actions) {
    StructuredError err = createError();
    err.suggestedAction = action;
    EXPECT_EQ(err.suggestedAction, action);
  }
}

TEST_F(StructuredErrorTest, SuggestedActionEmpty) {
  StructuredError err = createError();
  err.suggestedAction = "";
  EXPECT_TRUE(err.suggestedAction.empty());
}

// ===== Source Location Information =====

TEST_F(StructuredErrorTest, SourceFileAssignment) {
  StructuredError err = createError();
  EXPECT_EQ(err.sourceFile, "main.cpp");

  err.sourceFile = "other.h";
  EXPECT_EQ(err.sourceFile, "other.h");
}

TEST_F(StructuredErrorTest, SourceLineAssignment) {
  StructuredError err = createError();
  EXPECT_EQ(err.sourceLine, 42);

  err.sourceLine = 123;
  EXPECT_EQ(err.sourceLine, 123);
}

TEST_F(StructuredErrorTest, SourceLineZero) {
  StructuredError err = createError();
  err.sourceLine = 0;
  EXPECT_EQ(err.sourceLine, 0);
}

TEST_F(StructuredErrorTest, SourceLineNegativeValid) {
  StructuredError err = createError();
  err.sourceLine = -1;
  EXPECT_EQ(err.sourceLine, -1);
}

TEST_F(StructuredErrorTest, SourceFileLongPath) {
  StructuredError err = createError();
  err.sourceFile = "path/to/deeply/nested/source/file/with/long/name.cpp";
  EXPECT_EQ(err.sourceFile, "path/to/deeply/nested/source/file/with/long/name.cpp");
}

// ===== Context Information =====

TEST_F(StructuredErrorTest, ContextEmpty) {
  StructuredError err = createError();
  err.context.clear();
  EXPECT_TRUE(err.context.empty());
}

TEST_F(StructuredErrorTest, ContextSingleEntry) {
  StructuredError err = createError();
  err.context.push_back("Previous build succeeded");
  EXPECT_EQ(err.context.size(), 1);
  EXPECT_EQ(err.context[0], "Previous build succeeded");
}

TEST_F(StructuredErrorTest, ContextMultipleEntries) {
  StructuredError err = createError();
  err.context = {
      "Previous build succeeded",
      "Symbol was recently renamed",
      "Dependency graph is incomplete"};

  EXPECT_EQ(err.context.size(), 3);
  EXPECT_EQ(err.context[0], "Previous build succeeded");
  EXPECT_EQ(err.context[2], "Dependency graph is incomplete");
}

TEST_F(StructuredErrorTest, ContextAddMultiple) {
  StructuredError err = createError();
  for (int i = 0; i < 10; ++i) {
    err.context.push_back("Context line " + std::to_string(i));
  }
  EXPECT_EQ(err.context.size(), 10);
}

// ===== Timestamp Integration =====

TEST_F(StructuredErrorTest, TimestampDefault) {
  StructuredError err;
  // Default timestamp should be valid
  EXPECT_GE(err.timestamp.epochMs(), 0);
}

TEST_F(StructuredErrorTest, TimestampNow) {
  auto ts_before = Timestamp::now();
  StructuredError err = createError();
  auto ts_after = Timestamp::now();

  // Error timestamp should be between before and after
  EXPECT_LE(ts_before.epochMs(), err.timestamp.epochMs());
  EXPECT_LE(err.timestamp.epochMs(), ts_after.epochMs());
}

TEST_F(StructuredErrorTest, TimestampCustom) {
  StructuredError err;
  err.timestamp = Timestamp::fromEpochMs(1234567890000);
  EXPECT_EQ(err.timestamp.epochMs(), 1234567890000);
}

// ===== JSON Serialization =====

TEST_F(StructuredErrorTest, JsonSerializationBasic) {
  StructuredError err = createError();

  nlohmann::json j;
  to_json(j, err);

  EXPECT_EQ(j["error_type"], "LinkerError");
  EXPECT_EQ(j["message"], "Undefined reference to 'initialize'");
  EXPECT_EQ(j["severity"], 0.83);
  EXPECT_EQ(j["suggested_action"], "rebuild_deps");
}

TEST_F(StructuredErrorTest, JsonSerializationWithOptionalFields) {
  StructuredError err = createError();
  err.symbol = "my_symbol";
  err.sourceFile = "file.cpp";
  err.sourceLine = 50;

  nlohmann::json j;
  to_json(j, err);

  EXPECT_EQ(j["symbol"], "my_symbol");
  EXPECT_EQ(j["source_file"], "file.cpp");
  EXPECT_EQ(j["source_line"], 50);
}

TEST_F(StructuredErrorTest, JsonSerializationWithContext) {
  StructuredError err = createError();
  err.context = {"line 1", "line 2", "line 3"};

  nlohmann::json j;
  to_json(j, err);

  EXPECT_EQ(j["context"].size(), 3);
  EXPECT_EQ(j["context"][0], "line 1");
}

TEST_F(StructuredErrorTest, JsonDeserialization) {
  nlohmann::json j = {
      {"error_type", "ConfigError"},
      {"message", "Invalid configuration"},
      {"severity", 0.6},
      {"suggested_action", "update_config"}};

  StructuredError err;
  from_json(j, err);

  EXPECT_EQ(err.errorType, "ConfigError");
  EXPECT_EQ(err.message, "Invalid configuration");
  EXPECT_EQ(err.severity, 0.6);
  EXPECT_EQ(err.suggestedAction, "update_config");
}

TEST_F(StructuredErrorTest, JsonRoundTrip) {
  StructuredError err = createError();
  err.symbol = "test_symbol";
  err.context = {"context1", "context2"};

  nlohmann::json j;
  to_json(j, err);

  StructuredError err2;
  from_json(j, err2);

  EXPECT_EQ(err.errorType, err2.errorType);
  EXPECT_EQ(err.message, err2.message);
  EXPECT_EQ(err.symbol, err2.symbol);
  EXPECT_EQ(err.severity, err2.severity);
}

// ===== Deterministic Serialization =====

TEST_F(StructuredErrorTest, DeterministicJsonOutput) {
  StructuredError err = createError();

  nlohmann::json j1;
  to_json(j1, err);
  std::string json1 = j1.dump();

  nlohmann::json j2;
  to_json(j2, err);
  std::string json2 = j2.dump();

  EXPECT_EQ(json1, json2);
}

TEST_F(StructuredErrorTest, DeterministicSerialization) {
  StructuredError err = createError();

  for (int i = 0; i < 3; ++i) {
    nlohmann::json j;
    to_json(j, err);
    EXPECT_EQ(j["error_type"], "LinkerError");
    EXPECT_EQ(j["message"], "Undefined reference to 'initialize'");
  }
}

// ===== Large Error Messages =====

TEST_F(StructuredErrorTest, LargeErrorMessage) {
  StructuredError err = createError();
  std::string longMsg;
  for (int i = 0; i < 200; ++i) {
    longMsg += "This is a very long error message segment. ";
  }
  err.message = longMsg;

  EXPECT_EQ(err.message.length(), longMsg.length());

  nlohmann::json j;
  to_json(j, err);
  EXPECT_EQ(j["message"], longMsg);
}

TEST_F(StructuredErrorTest, LargeContextLines) {
  StructuredError err = createError();
  for (int i = 0; i < 50; ++i) {
    err.context.push_back("Context line " + std::to_string(i) +
                          ": additional diagnostic information");
  }

  EXPECT_EQ(err.context.size(), 50);

  nlohmann::json j;
  to_json(j, err);
  EXPECT_EQ(j["context"].size(), 50);
}

// ===== Stability Across Repeated Operations =====

TEST_F(StructuredErrorTest, StabilityMultipleSerialization) {
  StructuredError err = createError();

  for (int i = 0; i < 5; ++i) {
    nlohmann::json j;
    to_json(j, err);
    EXPECT_EQ(j["error_type"], "LinkerError");
    EXPECT_NEAR(j["severity"].get<double>(), 0.83, 0.001);
  }
}

TEST_F(StructuredErrorTest, StabilityFieldModification) {
  StructuredError err = createError();

  err.severity = 0.75;
  EXPECT_EQ(err.severity, 0.75);

  err.severity = 0.75;
  EXPECT_EQ(err.severity, 0.75);
}

// ===== Equality Comparison =====

TEST_F(StructuredErrorTest, EqualityIdentical) {
  StructuredError err1 = createError();
  StructuredError err2 = createError();

  EXPECT_EQ(err1, err2);
}

TEST_F(StructuredErrorTest, EqualityDifferentType) {
  StructuredError err1 = createError();
  StructuredError err2 = createError();
  err2.errorType = "DifferentError";

  EXPECT_NE(err1, err2);
}

TEST_F(StructuredErrorTest, EqualityDifferentMessage) {
  StructuredError err1 = createError();
  StructuredError err2 = createError();
  err2.message = "Different message";

  EXPECT_NE(err1, err2);
}

TEST_F(StructuredErrorTest, EqualityDifferentSeverity) {
  StructuredError err1 = createError();
  StructuredError err2 = createError();
  err2.severity = 0.5;

  EXPECT_NE(err1, err2);
}

// ===== Edge Cases =====

TEST_F(StructuredErrorTest, EmptyErrorType) {
  StructuredError err = createError();
  err.errorType = "";
  EXPECT_TRUE(err.errorType.empty());
}

TEST_F(StructuredErrorTest, EmptyMessage) {
  StructuredError err = createError();
  err.message = "";
  EXPECT_TRUE(err.message.empty());
}

TEST_F(StructuredErrorTest, SourceLineVeryLarge) {
  StructuredError err = createError();
  err.sourceLine = 999999;
  EXPECT_EQ(err.sourceLine, 999999);
}

}  // namespace ultra::types
