// =====================================================
// Structured Response Tests
// Tests for StructuredResponse: external API response wrapping
// =====================================================

#include <gtest/gtest.h>
#include "api/StructuredResponse.h"
#include "types/Timestamp.h"
#include <external/json.hpp>

namespace ultra::api {

class StructuredResponseTest : public ::testing::Test {
 protected:
  StructuredResponse createResponse() {
    StructuredResponse resp;
    resp.source = "github";
    resp.changeType = "pull_request";
    resp.timestamp = ultra::types::Timestamp::now();
    resp.success = true;
    return resp;
  }
};

// ===== Basic Response Construction =====

TEST_F(StructuredResponseTest, DefaultConstruction) {
  StructuredResponse resp;
  EXPECT_TRUE(resp.source.empty());
  EXPECT_TRUE(resp.changeType.empty());
  EXPECT_EQ(resp.impactScore, 0.0);
  EXPECT_FALSE(resp.success);
  EXPECT_TRUE(resp.errorMessage.empty());
}

TEST_F(StructuredResponseTest, SuccessResponseConstruction) {
  StructuredResponse resp = createResponse();
  EXPECT_EQ(resp.source, "github");
  EXPECT_EQ(resp.changeType, "pull_request");
  EXPECT_TRUE(resp.success);
  EXPECT_EQ(resp.impactScore, 0.0);
}

TEST_F(StructuredResponseTest, ErrorResponseConstruction) {
  StructuredResponse resp;
  resp.source = "jira";
  resp.changeType = "issue";
  resp.success = false;
  resp.errorMessage = "Authentication failed";
  resp.impactScore = 0.0;

  EXPECT_EQ(resp.source, "jira");
  EXPECT_FALSE(resp.success);
  EXPECT_EQ(resp.errorMessage, "Authentication failed");
}

// ===== Field Manipulation =====

TEST_F(StructuredResponseTest, SetAllFields) {
  StructuredResponse resp;
  resp.source = "gitlab";
  resp.changeType = "merge_request";
  resp.success = true;
  resp.impactScore = 0.85;
  resp.errorMessage = "";

  EXPECT_EQ(resp.source, "gitlab");
  EXPECT_EQ(resp.changeType, "merge_request");
  EXPECT_TRUE(resp.success);
  EXPECT_NEAR(resp.impactScore, 0.85, 0.001);
}

TEST_F(StructuredResponseTest, ModifyFieldsAfterConstruction) {
  StructuredResponse resp = createResponse();

  resp.impactScore = 0.45;
  EXPECT_NEAR(resp.impactScore, 0.45, 0.001);

  resp.errorMessage = "Test error";
  EXPECT_EQ(resp.errorMessage, "Test error");

  resp.success = false;
  EXPECT_FALSE(resp.success);
}

// ===== JSON Data Handling =====

TEST_F(StructuredResponseTest, JsonDataEmpty) {
  StructuredResponse resp = createResponse();
  EXPECT_TRUE(resp.data.is_null());
}

TEST_F(StructuredResponseTest, JsonDataSimpleObject) {
  StructuredResponse resp = createResponse();
  resp.data = nlohmann::json{
      {"id", 123},
      {"title", "Add feature X"},
      {"status", "open"}};

  EXPECT_EQ(resp.data["id"], 123);
  EXPECT_EQ(resp.data["title"], "Add feature X");
  EXPECT_EQ(resp.data["status"], "open");
}

TEST_F(StructuredResponseTest, JsonDataNestedStructure) {
  StructuredResponse resp = createResponse();
  resp.data = nlohmann::json{
      {"repository", nlohmann::json{
          {"name", "myrepo"},
          {"owner", "myuser"},
          {"forks", 42}}},
      {"metrics", nlohmann::json{
          {"stars", 1000},
          {"watchers", 50}}}};

  EXPECT_EQ(resp.data["repository"]["name"], "myrepo");
  EXPECT_EQ(resp.data["metrics"]["stars"], 1000);
}

TEST_F(StructuredResponseTest, JsonDataArray) {
  StructuredResponse resp = createResponse();
  resp.data = nlohmann::json::array();
  resp.data.push_back(nlohmann::json{{"id", 1}, {"name", "file1.cpp"}});
  resp.data.push_back(nlohmann::json{{"id", 2}, {"name", "file2.cpp"}});

  EXPECT_EQ(resp.data.size(), 2);
  EXPECT_EQ(resp.data[0]["name"], "file1.cpp");
  EXPECT_EQ(resp.data[1]["name"], "file2.cpp");
}

// ===== Impact Score Validation =====

TEST_F(StructuredResponseTest, ImpactScoreLow) {
  StructuredResponse resp = createResponse();
  resp.impactScore = 0.1;
  EXPECT_NEAR(resp.impactScore, 0.1, 0.001);
}

TEST_F(StructuredResponseTest, ImpactScoreHigh) {
  StructuredResponse resp = createResponse();
  resp.impactScore = 0.95;
  EXPECT_NEAR(resp.impactScore, 0.95, 0.001);
}

TEST_F(StructuredResponseTest, ImpactScoreMidrange) {
  StructuredResponse resp = createResponse();
  resp.impactScore = 0.5;
  EXPECT_NEAR(resp.impactScore, 0.5, 0.001);
}

// ===== Source and Type Variations =====

TEST_F(StructuredResponseTest, VariousSourceNames) {
  std::vector<std::string> sources = {"github", "gitlab", "bitbucket", "jira", "asana"};
  for (const auto& src : sources) {
    StructuredResponse resp = createResponse();
    resp.source = src;
    EXPECT_EQ(resp.source, src);
  }
}

TEST_F(StructuredResponseTest, VariousChangeTypes) {
  std::vector<std::string> types = {
      "pull_request", "commit", "issue", "merge_request", "push", "release"};
  for (const auto& type : types) {
    StructuredResponse resp = createResponse();
    resp.changeType = type;
    EXPECT_EQ(resp.changeType, type);
  }
}

// ===== Large Payload Handling =====

TEST_F(StructuredResponseTest, LargeJsonPayload) {
  StructuredResponse resp = createResponse();
  resp.data = nlohmann::json::object();

  // Add 100 fields to JSON
  for (int i = 0; i < 100; ++i) {
    resp.data["field_" + std::to_string(i)] = i * 2;
  }

  EXPECT_EQ(resp.data.size(), 100);
  EXPECT_EQ(resp.data["field_50"], 100);
}

TEST_F(StructuredResponseTest, LargeLongErrorMessage) {
  StructuredResponse resp = createResponse();
  std::string longMsg;
  for (int i = 0; i < 500; ++i) {
    longMsg += "error message segment ";
  }
  resp.errorMessage = longMsg;

  EXPECT_EQ(resp.errorMessage.length(), longMsg.length());
}

// ===== Timestamp Integration =====

TEST_F(StructuredResponseTest, TimestampDefault) {
  StructuredResponse resp;
  // Default timestamp should be valid
  EXPECT_GE(resp.timestamp.epochMs(), 0);
}

TEST_F(StructuredResponseTest, TimestampNow) {
  StructuredResponse resp = createResponse();
  auto ts = ultra::types::Timestamp::now();

  // Both timestamps should be close (within reasonable time window)
  EXPECT_LE(resp.timestamp.epochMs(), ts.epochMs());
}

TEST_F(StructuredResponseTest, TimestampCustom) {
  StructuredResponse resp;
  resp.timestamp = ultra::types::Timestamp::fromEpochMs(1000000);
  EXPECT_EQ(resp.timestamp.epochMs(), 1000000);
}

// ===== Deterministic Behavior =====

TEST_F(StructuredResponseTest, DeterministicResponseCreation) {
  StructuredResponse resp1 = createResponse();
  StructuredResponse resp2 = createResponse();

  // Same setup should produce same field values
  EXPECT_EQ(resp1.source, resp2.source);
  EXPECT_EQ(resp1.changeType, resp2.changeType);
  EXPECT_EQ(resp1.success, resp2.success);
}

TEST_F(StructuredResponseTest, DeterministicJsonData) {
  StructuredResponse resp;
  resp.data = nlohmann::json{
      {"id", 42},
      {"value", "test"}};

  auto json1 = resp.data.dump();
  auto json2 = resp.data.dump();

  // Serialization should be identical
  EXPECT_EQ(json1, json2);
}

// ===== Success/Error Response Patterns =====

TEST_F(StructuredResponseTest, SuccessResponse) {
  StructuredResponse resp = createResponse();
  resp.data = nlohmann::json{{"status", "completed"}};

  EXPECT_TRUE(resp.success);
  EXPECT_TRUE(resp.errorMessage.empty());
  EXPECT_EQ(resp.data["status"], "completed");
}

TEST_F(StructuredResponseTest, ErrorResponse) {
  StructuredResponse resp = createResponse();
  resp.success = false;
  resp.errorMessage = "Failed to fetch data";
  resp.data = nlohmann::json{{"reason", "unauthorized"}};

  EXPECT_FALSE(resp.success);
  EXPECT_EQ(resp.errorMessage, "Failed to fetch data");
  EXPECT_EQ(resp.data["reason"], "unauthorized");
}

TEST_F(StructuredResponseTest, PartialSuccessResponse) {
  StructuredResponse resp = createResponse();
  resp.success = true;
  resp.errorMessage = "Some items failed";
  resp.data = nlohmann::json{
      {"total", 100},
      {"successful", 97},
      {"failed", 3}};

  EXPECT_TRUE(resp.success);
  EXPECT_FALSE(resp.errorMessage.empty());
  EXPECT_EQ(resp.data["successful"], 97);
}

// ===== Stability Across Repeated Operations =====

TEST_F(StructuredResponseTest, StabilityJsonAccess) {
  StructuredResponse resp = createResponse();
  resp.data = nlohmann::json{{"key", "value"}};

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(resp.data["key"], "value");
  }
}

TEST_F(StructuredResponseTest, StabilityFieldModification) {
  StructuredResponse resp = createResponse();

  for (int i = 0; i < 3; ++i) {
    resp.impactScore = 0.7;
    EXPECT_NEAR(resp.impactScore, 0.7, 0.001);
  }
}

// ===== Edge Cases =====

TEST_F(StructuredResponseTest, EmptySourceAndType) {
  StructuredResponse resp;
  resp.source = "";
  resp.changeType = "";
  resp.success = false;

  EXPECT_TRUE(resp.source.empty());
  EXPECT_TRUE(resp.changeType.empty());
  EXPECT_FALSE(resp.success);
}

TEST_F(StructuredResponseTest, ZeroImpactScore) {
  StructuredResponse resp = createResponse();
  resp.impactScore = 0.0;
  EXPECT_EQ(resp.impactScore, 0.0);
}

TEST_F(StructuredResponseTest, MaxImpactScore) {
  StructuredResponse resp = createResponse();
  resp.impactScore = 1.0;
  EXPECT_EQ(resp.impactScore, 1.0);
}

TEST_F(StructuredResponseTest, VerySmallImpactScore) {
  StructuredResponse resp = createResponse();
  resp.impactScore = 0.001;
  EXPECT_NEAR(resp.impactScore, 0.001, 0.0001);
}

}  // namespace ultra::api
