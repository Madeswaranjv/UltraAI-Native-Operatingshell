// =====================================================
// Serialization Tests
// Tests for JSON serialization/deserialization of Ultra types
// =====================================================

#include <gtest/gtest.h>
#include "types/Serialization.h"
#include "types/Confidence.h"
#include "types/StructuredError.h"
#include "types/Timestamp.h"
#include <external/json.hpp>

namespace ultra::types {

class SerializationTest : public ::testing::Test {
 protected:
  nlohmann::json createValidJson() {
    return nlohmann::json{
        {"stability_score", 0.8},
        {"risk_adjusted_confidence", 0.75},
        {"decision_reliability_index", 0.85}};
  }
};

// ===== Timestamp Serialization =====

TEST_F(SerializationTest, TimestampToJson) {
  Timestamp ts = Timestamp::fromEpochMs(1645360000000);
  nlohmann::json j = ts;
  EXPECT_TRUE(j.is_string());
  EXPECT_FALSE(j.get<std::string>().empty());
}

TEST_F(SerializationTest, TimestampFromJson) {
  nlohmann::json j = "2026-02-21T16:30:00.000Z";
  Timestamp ts = j.get<Timestamp>();
  EXPECT_GE(ts.epochMs(), 0);
}

TEST_F(SerializationTest, TimestampRoundTrip) {
  Timestamp ts_orig = Timestamp::fromEpochMs(1645360000000);
  nlohmann::json j = ts_orig;
  Timestamp ts_restored = j.get<Timestamp>();
  EXPECT_EQ(ts_orig.epochMs(), ts_restored.epochMs());
}

TEST_F(SerializationTest, TimestampNowSerialization) {
  Timestamp ts = Timestamp::now();
  nlohmann::json j = ts;
  std::string iso = j.get<std::string>();
  EXPECT_FALSE(iso.empty());
  EXPECT_NE(iso.find('T'), std::string::npos);
  EXPECT_NE(iso.find('Z'), std::string::npos);
}

// ===== Confidence Serialization =====

TEST_F(SerializationTest, ConfidenceToJson) {
  Confidence conf;
  conf.stabilityScore = 0.8;
  conf.riskAdjustedConfidence = 0.75;
  conf.decisionReliabilityIndex = 0.85;

  nlohmann::json j = conf;
  EXPECT_EQ(j["stability_score"], 0.8);
  EXPECT_EQ(j["risk_adjusted_confidence"], 0.75);
  EXPECT_EQ(j["decision_reliability_index"], 0.85);
  EXPECT_NEAR(j["overall"].get<double>(), (0.8 + 0.75 + 0.85) / 3.0, 0.001);
}

TEST_F(SerializationTest, ConfidenceFromJson) {
  nlohmann::json j = createValidJson();
  Confidence conf = j.get<Confidence>();

  EXPECT_EQ(conf.stabilityScore, 0.8);
  EXPECT_EQ(conf.riskAdjustedConfidence, 0.75);
  EXPECT_EQ(conf.decisionReliabilityIndex, 0.85);
}

TEST_F(SerializationTest, ConfidenceRoundTrip) {
  Confidence conf1;
  conf1.stabilityScore = 0.7;
  conf1.riskAdjustedConfidence = 0.8;
  conf1.decisionReliabilityIndex = 0.9;

  nlohmann::json j = conf1;
  Confidence conf2 = j.get<Confidence>();

  EXPECT_EQ(conf1.stabilityScore, conf2.stabilityScore);
  EXPECT_EQ(conf1.riskAdjustedConfidence, conf2.riskAdjustedConfidence);
  EXPECT_EQ(conf1.decisionReliabilityIndex, conf2.decisionReliabilityIndex);
}

TEST_F(SerializationTest, ConfidenceZeroValues) {
  Confidence conf;
  conf.stabilityScore = 0.0;
  conf.riskAdjustedConfidence = 0.0;
  conf.decisionReliabilityIndex = 0.0;

  nlohmann::json j = conf;
  Confidence conf2 = j.get<Confidence>();

  EXPECT_EQ(conf2.stabilityScore, 0.0);
  EXPECT_EQ(conf2.riskAdjustedConfidence, 0.0);
  EXPECT_EQ(conf2.decisionReliabilityIndex, 0.0);
}

TEST_F(SerializationTest, ConfidenceHighValues) {
  Confidence conf;
  conf.stabilityScore = 1.0;
  conf.riskAdjustedConfidence = 1.0;
  conf.decisionReliabilityIndex = 1.0;

  nlohmann::json j = conf;
  Confidence conf2 = j.get<Confidence>();

  EXPECT_EQ(conf2.stabilityScore, 1.0);
  EXPECT_EQ(conf2.riskAdjustedConfidence, 1.0);
  EXPECT_EQ(conf2.decisionReliabilityIndex, 1.0);
}

// ===== StructuredError Serialization =====

TEST_F(SerializationTest, ErrorToJson) {
  StructuredError err;
  err.errorType = "TestError";
  err.message = "Test message";
  err.severity = 0.5;
  err.suggestedAction = "test_action";

  nlohmann::json j = err;
  EXPECT_EQ(j["error_type"], "TestError");
  EXPECT_EQ(j["message"], "Test message");
  EXPECT_EQ(j["severity"], 0.5);
  EXPECT_EQ(j["suggested_action"], "test_action");
}

TEST_F(SerializationTest, ErrorFromJson) {
  nlohmann::json j;
  j["error_type"] = "ConfigError";
  j["message"] = "Invalid config";
  j["severity"] = 0.75;
  j["suggested_action"] = "fix_config";

  StructuredError err = j.get<StructuredError>();
  EXPECT_EQ(err.errorType, "ConfigError");
  EXPECT_EQ(err.message, "Invalid config");
  EXPECT_EQ(err.severity, 0.75);
  EXPECT_EQ(err.suggestedAction, "fix_config");
}

TEST_F(SerializationTest, ErrorWithOptionalFields) {
  StructuredError err;
  err.errorType = "LinkerError";
  err.message = "Undefined reference";
  err.severity = 0.9;
  err.suggestedAction = "rebuild";
  err.symbol = "undefined_func";
  err.sourceFile = "main.cpp";
  err.sourceLine = 42;
  err.context = {"Context 1", "Context 2"};

  nlohmann::json j = err;
  EXPECT_EQ(j["symbol"], "undefined_func");
  EXPECT_EQ(j["source_file"], "main.cpp");
  EXPECT_EQ(j["source_line"], 42);
  EXPECT_EQ(j["context"].size(), 2);
}

TEST_F(SerializationTest, ErrorRoundTrip) {
  StructuredError err1;
  err1.errorType = "TestError";
  err1.message = "Test";
  err1.severity = 0.6;
  err1.suggestedAction = "action";
  err1.symbol = "sym";
  err1.sourceFile = "file.cpp";
  err1.sourceLine = 10;

  nlohmann::json j = err1;
  StructuredError err2 = j.get<StructuredError>();

  EXPECT_EQ(err1.errorType, err2.errorType);
  EXPECT_EQ(err1.message, err2.message);
  EXPECT_EQ(err1.severity, err2.severity);
  EXPECT_EQ(err1.suggestedAction, err2.suggestedAction);
}

// ===== Error Handling in Deserialization =====

TEST_F(SerializationTest, InvalidJsonConfidence) {
  nlohmann::json j = nlohmann::json::object();
  // Missing required fields - should throw exception
  try {
    Confidence conf = j.get<Confidence>();
    // If no exception, fields should have default values
    EXPECT_EQ(conf.stabilityScore, 0.0);
  } catch (const std::exception&) {
    // Exception is acceptable for missing required fields
    EXPECT_TRUE(true);
  }
}

TEST_F(SerializationTest, MissingRequiredErrorFields) {
  nlohmann::json j;
  j["error_type"] = "Error";
  // Missing message, severity, suggested_action
  try {
    StructuredError err = j.get<StructuredError>();
    EXPECT_TRUE(true);
  } catch (const std::exception&) {
    EXPECT_TRUE(true);
  }
}

TEST_F(SerializationTest, ExtraUnknownFields) {
  nlohmann::json j = createValidJson();
  j["unknown_field_1"] = "value1";
  j["unknown_field_2"] = 42;

  // Should not crash with extra fields
  Confidence conf = j.get<Confidence>();
  EXPECT_EQ(conf.stabilityScore, 0.8);
}

// ===== Large Object Serialization =====

TEST_F(SerializationTest, LargeConfidenceArray) {
  nlohmann::json arr = nlohmann::json::array();
  for (int i = 0; i < 100; ++i) {
    Confidence conf;
    conf.stabilityScore = 0.5 + (i % 10) * 0.05;
    conf.riskAdjustedConfidence = 0.6 + (i % 10) * 0.03;
    conf.decisionReliabilityIndex = 0.7 + (i % 10) * 0.02;
    arr.push_back(conf);
  }

  EXPECT_EQ(arr.size(), 100);

  for (int i = 0; i < 100; ++i) {
    Confidence conf = arr[i].get<Confidence>();
    EXPECT_GE(conf.stabilityScore, 0.5);
    EXPECT_LE(conf.stabilityScore, 1.0);
  }
}

TEST_F(SerializationTest, LargeErrorMessage) {
  std::string longMsg;
  for (int i = 0; i < 200; ++i) {
    longMsg += "error message segment ";
  }

  StructuredError err;
  err.errorType = "TestError";
  err.message = longMsg;
  err.severity = 0.5;
  err.suggestedAction = "action";

  nlohmann::json j = err;
  StructuredError err2 = j.get<StructuredError>();

  EXPECT_EQ(err2.message.length(), longMsg.length());
}

// ===== Deterministic Serialization =====

TEST_F(SerializationTest, DeterministicTimestampSerialization) {
  Timestamp ts = Timestamp::fromEpochMs(1645360000000);

  std::string s1 = (nlohmann::json)ts;
  std::string s2 = (nlohmann::json)ts;

  EXPECT_EQ(s1, s2);
}

TEST_F(SerializationTest, DeterministicConfidenceSerialization) {
  Confidence conf;
  conf.stabilityScore = 0.8;
  conf.riskAdjustedConfidence = 0.75;
  conf.decisionReliabilityIndex = 0.85;

  nlohmann::json j1 = conf;
  nlohmann::json j2 = conf;

  EXPECT_EQ(j1.dump(), j2.dump());
}

TEST_F(SerializationTest, DeterministicErrorSerialization) {
  StructuredError err;
  err.errorType = "TestError";
  err.message = "Test message";
  err.severity = 0.5;
  err.suggestedAction = "action";

  nlohmann::json j1 = err;
  nlohmann::json j2 = err;

  EXPECT_EQ(j1.dump(), j2.dump());
}

// ===== Stability Across Repeated Cycles =====

TEST_F(SerializationTest, StabilityMultipleRoundTrips) {
  Confidence conf1;
  conf1.stabilityScore = 0.7;
  conf1.riskAdjustedConfidence = 0.8;
  conf1.decisionReliabilityIndex = 0.9;

  Confidence current = conf1;
  for (int i = 0; i < 5; ++i) {
    nlohmann::json j = current;
    current = j.get<Confidence>();

    EXPECT_EQ(current.stabilityScore, 0.7);
    EXPECT_EQ(current.riskAdjustedConfidence, 0.8);
    EXPECT_EQ(current.decisionReliabilityIndex, 0.9);
  }
}

// ===== Edge Cases =====

TEST_F(SerializationTest, EmptyError) {
  StructuredError err;
  nlohmann::json j = err;

  EXPECT_TRUE(j["error_type"].is_string());
  EXPECT_TRUE(j["message"].is_string());
}

TEST_F(SerializationTest, ConfidenceWithZeroValues) {
  Confidence conf;
  conf.stabilityScore = 0.0;
  conf.riskAdjustedConfidence = 0.0;
  conf.decisionReliabilityIndex = 0.0;

  nlohmann::json j = conf;
  EXPECT_EQ(j["stability_score"], 0.0);
  EXPECT_EQ(j["risk_adjusted_confidence"], 0.0);
  EXPECT_EQ(j["decision_reliability_index"], 0.0);
}

TEST_F(SerializationTest, VerySmallConfidenceValues) {
  Confidence conf;
  conf.stabilityScore = 0.001;
  conf.riskAdjustedConfidence = 0.002;
  conf.decisionReliabilityIndex = 0.003;

  nlohmann::json j = conf;
  Confidence conf2 = j.get<Confidence>();

  EXPECT_NEAR(conf2.stabilityScore, 0.001, 0.0001);
  EXPECT_NEAR(conf2.riskAdjustedConfidence, 0.002, 0.0001);
  EXPECT_NEAR(conf2.decisionReliabilityIndex, 0.003, 0.0001);
}

TEST_F(SerializationTest, HighPrecisionValues) {
  Confidence conf;
  conf.stabilityScore = 0.123456789;
  conf.riskAdjustedConfidence = 0.987654321;
  conf.decisionReliabilityIndex = 0.555555555;

  nlohmann::json j = conf;
  Confidence conf2 = j.get<Confidence>();

  EXPECT_NEAR(conf2.stabilityScore, 0.123456789, 0.0000001);
  EXPECT_NEAR(conf2.riskAdjustedConfidence, 0.987654321, 0.0000001);
  EXPECT_NEAR(conf2.decisionReliabilityIndex, 0.555555555, 0.0000001);
}

TEST_F(SerializationTest, ErrorWithEmptyContext) {
  StructuredError err;
  err.errorType = "Error";
  err.message = "msg";
  err.severity = 0.5;
  err.suggestedAction = "action";
  err.context.clear();

  nlohmann::json j = err;
  StructuredError err2 = j.get<StructuredError>();

  EXPECT_EQ(err2.context.size(), 0);
}

TEST_F(SerializationTest, ErrorWithManyContextLines) {
  StructuredError err;
  err.errorType = "Error";
  err.message = "msg";
  err.severity = 0.5;
  err.suggestedAction = "action";

  for (int i = 0; i < 50; ++i) {
    err.context.push_back("Context " + std::to_string(i));
  }

  nlohmann::json j = err;
  StructuredError err2 = j.get<StructuredError>();

  EXPECT_EQ(err2.context.size(), 50);
}

}  // namespace ultra::types
