// =====================================================
// API Registry Tests
// Tests for ApiRegistry: connector registration and retrieval
// =====================================================

#include <gtest/gtest.h>
#include "api/ApiRegistry.h"
#include "api/IApiConnector.h"
#include "api/StructuredResponse.h"

#include <memory>
#include <string>
#include <vector>
#include <algorithm>

namespace ultra::api {

// Mock implementation of IApiConnector for testing
class MockConnector : public IApiConnector {
 private:
  std::string name_;
  bool configured_{false};

 public:
  explicit MockConnector(const std::string& name) : name_(name) {}

  bool isConfigured() const override { return configured_; }

  StructuredResponse query(const std::string& /*requestPath*/) const override {
    StructuredResponse resp;
    resp.source = name_;
    resp.changeType = "test_query";
    resp.success = true;
    return resp;
  }

  void configure(const std::string& configStr) override {
    if (!configStr.empty()) {
      configured_ = true;
    }
  }

  std::string getName() const override { return name_; }
};

class ApiRegistryTest : public ::testing::Test {
 protected:
  ApiRegistry registry_;
};

// ===== Basic Registration Tests =====

TEST_F(ApiRegistryTest, RegisterSingleConnector) {
  auto conn = std::make_unique<MockConnector>("github");
  registry_.registerConnector(std::move(conn));

  IApiConnector* retrieved = registry_.getConnector("github");
  ASSERT_NE(retrieved, nullptr);
  EXPECT_EQ(retrieved->getName(), "github");
}

TEST_F(ApiRegistryTest, RegisterMultipleConnectors) {
  registry_.registerConnector(std::make_unique<MockConnector>("github"));
  registry_.registerConnector(std::make_unique<MockConnector>("jira"));
  registry_.registerConnector(std::make_unique<MockConnector>("gitlab"));

  EXPECT_NE(registry_.getConnector("github"), nullptr);
  EXPECT_NE(registry_.getConnector("jira"), nullptr);
  EXPECT_NE(registry_.getConnector("gitlab"), nullptr);
}

TEST_F(ApiRegistryTest, DuplicateRegistrationOverwrites) {
  registry_.registerConnector(std::make_unique<MockConnector>("github"));
  registry_.registerConnector(std::make_unique<MockConnector>("github"));

  // Should have exactly one connector named "github"
  std::vector<std::string> names = registry_.listConnectors();
  int count = 0;
  for (const auto& name : names) {
    if (name == "github") count++;
  }
  EXPECT_EQ(count, 1);
}

TEST_F(ApiRegistryTest, RetrieveNonexistentConnector) {
  IApiConnector* connector = registry_.getConnector("nonexistent");
  EXPECT_EQ(connector, nullptr);
}

TEST_F(ApiRegistryTest, RetrieveFromEmptyRegistry) {
  IApiConnector* connector = registry_.getConnector("any");
  EXPECT_EQ(connector, nullptr);
}

// ===== List Connectors Tests =====

TEST_F(ApiRegistryTest, ListConnectorsEmpty) {
  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_TRUE(names.empty());
}

TEST_F(ApiRegistryTest, ListConnectorsSingle) {
  registry_.registerConnector(std::make_unique<MockConnector>("github"));

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 1);
  EXPECT_EQ(names[0], "github");
}

TEST_F(ApiRegistryTest, ListConnectorsMultiple) {
  registry_.registerConnector(std::make_unique<MockConnector>("github"));
  registry_.registerConnector(std::make_unique<MockConnector>("jira"));
  registry_.registerConnector(std::make_unique<MockConnector>("gitlab"));

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 3);

  // Check all names are present (order not guaranteed)
  std::sort(names.begin(), names.end());
  EXPECT_TRUE(std::find(names.begin(), names.end(), "github") != names.end());
  EXPECT_TRUE(std::find(names.begin(), names.end(), "jira") != names.end());
  EXPECT_TRUE(std::find(names.begin(), names.end(), "gitlab") != names.end());
}

TEST_F(ApiRegistryTest, ListConnectorsAfterDuplicateRegistration) {
  registry_.registerConnector(std::make_unique<MockConnector>("github"));
  registry_.registerConnector(std::make_unique<MockConnector>("github"));

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 1);
}

// ===== Large Registry Tests =====

TEST_F(ApiRegistryTest, LargeRegistry30Endpoints) {
  // Register 30 connectors
  for (int i = 0; i < 30; ++i) {
    std::string name = "connector_" + std::to_string(i);
    registry_.registerConnector(std::make_unique<MockConnector>(name));
  }

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 30);

  // Verify all are retrievable
  for (int i = 0; i < 30; ++i) {
    std::string name = "connector_" + std::to_string(i);
    EXPECT_NE(registry_.getConnector(name), nullptr);
  }
}

TEST_F(ApiRegistryTest, LargeRegistry100Endpoints) {
  // Register 100 connectors
  for (int i = 0; i < 100; ++i) {
    std::string name = "ep_" + std::to_string(i);
    registry_.registerConnector(std::make_unique<MockConnector>(name));
  }

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 100);

  // Sample retrieval across range
  EXPECT_NE(registry_.getConnector("ep_0"), nullptr);
  EXPECT_NE(registry_.getConnector("ep_49"), nullptr);
  EXPECT_NE(registry_.getConnector("ep_99"), nullptr);
}

// ===== Deterministic Behavior Tests =====

TEST_F(ApiRegistryTest, DeterministicRetrieval) {
  registry_.registerConnector(std::make_unique<MockConnector>("service_a"));
  registry_.registerConnector(std::make_unique<MockConnector>("service_b"));
  registry_.registerConnector(std::make_unique<MockConnector>("service_c"));

  // Retrieve multiple times - should always succeed/fail the same way
  for (int iter = 0; iter < 5; ++iter) {
    EXPECT_NE(registry_.getConnector("service_a"), nullptr);
    EXPECT_NE(registry_.getConnector("service_b"), nullptr);
    EXPECT_NE(registry_.getConnector("service_c"), nullptr);
    EXPECT_EQ(registry_.getConnector("nonexistent"), nullptr);
  }
}

TEST_F(ApiRegistryTest, DeterministicListing) {
  registry_.registerConnector(std::make_unique<MockConnector>("first"));
  registry_.registerConnector(std::make_unique<MockConnector>("second"));
  registry_.registerConnector(std::make_unique<MockConnector>("third"));

  // List multiple times - count should be consistent
  for (int iter = 0; iter < 3; ++iter) {
    std::vector<std::string> names = registry_.listConnectors();
    EXPECT_EQ(names.size(), 3);
  }
}

// ===== Stability Across Repeated Registration =====

TEST_F(ApiRegistryTest, StabilityMultipleRegistrations) {
  // Register same connector multiple times
  for (int i = 0; i < 5; ++i) {
    registry_.registerConnector(std::make_unique<MockConnector>("stable_service"));
  }

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 1);
  EXPECT_NE(registry_.getConnector("stable_service"), nullptr);
}

TEST_F(ApiRegistryTest, StabilityInterleavedRegistrationAndRetrieval) {
  registry_.registerConnector(std::make_unique<MockConnector>("alpha"));
  EXPECT_NE(registry_.getConnector("alpha"), nullptr);

  registry_.registerConnector(std::make_unique<MockConnector>("beta"));
  EXPECT_NE(registry_.getConnector("alpha"), nullptr);
  EXPECT_NE(registry_.getConnector("beta"), nullptr);

  registry_.registerConnector(std::make_unique<MockConnector>("gamma"));
  EXPECT_NE(registry_.getConnector("alpha"), nullptr);
  EXPECT_NE(registry_.getConnector("beta"), nullptr);
  EXPECT_NE(registry_.getConnector("gamma"), nullptr);

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 3);
}

// ===== Registry Integrity Validation =====

TEST_F(ApiRegistryTest, IntegrityAfterManyOperations) {
  // Register, list, retrieve in various patterns
  for (int i = 0; i < 10; ++i) {
    std::string name = "service_" + std::to_string(i);
    registry_.registerConnector(std::make_unique<MockConnector>(name));
  }

  std::vector<std::string> names1 = registry_.listConnectors();
  std::vector<std::string> names2 = registry_.listConnectors();

  EXPECT_EQ(names1.size(), names2.size());
  std::sort(names1.begin(), names1.end());
  std::sort(names2.begin(), names2.end());
  EXPECT_EQ(names1, names2);
}

TEST_F(ApiRegistryTest, IntegrityConsistentRetrieval) {
  std::vector<std::string> expected_names = {"svc_1", "svc_2", "svc_3"};

  for (const auto& name : expected_names) {
    registry_.registerConnector(std::make_unique<MockConnector>(name));
  }

  for (const auto& name : expected_names) {
    IApiConnector* ptr1 = registry_.getConnector(name);
    IApiConnector* ptr2 = registry_.getConnector(name);
    EXPECT_NE(ptr1, nullptr);
    EXPECT_NE(ptr2, nullptr);
    // Same name should retrieve same underlying connector
    EXPECT_EQ(ptr1->getName(), ptr2->getName());
  }
}

// ===== Null/Empty Input Handling =====

TEST_F(ApiRegistryTest, RegisterNullptrConnector) {
  // Registering nullptr should not crash, just skip
  registry_.registerConnector(nullptr);
  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 0);
}

TEST_F(ApiRegistryTest, GetConnectorWithEmptyString) {
  IApiConnector* connector = registry_.getConnector("");
  EXPECT_EQ(connector, nullptr);
}

// ===== Connector Retrieval and Usage =====

TEST_F(ApiRegistryTest, RetrievedConnectorIsCallable) {
  registry_.registerConnector(std::make_unique<MockConnector>("github"));

  IApiConnector* connector = registry_.getConnector("github");
  ASSERT_NE(connector, nullptr);

  // Should be able to call methods on retrieved connector
  EXPECT_EQ(connector->getName(), "github");

  connector->configure("token");
  EXPECT_TRUE(connector->isConfigured());
}

TEST_F(ApiRegistryTest, MultipleRetrievedConnectorsIndependent) {
  registry_.registerConnector(std::make_unique<MockConnector>("conn_a"));
  registry_.registerConnector(std::make_unique<MockConnector>("conn_b"));

  IApiConnector* a = registry_.getConnector("conn_a");
  IApiConnector* b = registry_.getConnector("conn_b");

  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  a->configure("config_a");
  EXPECT_TRUE(a->isConfigured());
  EXPECT_FALSE(b->isConfigured());
}

// ===== Special Characters and Edge Cases =====

TEST_F(ApiRegistryTest, RegisterConnectorWithSpecialCharacters) {
  std::string name_with_dashes = "my-service-connector";
  std::string name_with_underscores = "my_service_connector";
  std::string name_with_dots = "my.service.connector";

  registry_.registerConnector(std::make_unique<MockConnector>(name_with_dashes));
  registry_.registerConnector(std::make_unique<MockConnector>(name_with_underscores));
  registry_.registerConnector(std::make_unique<MockConnector>(name_with_dots));

  EXPECT_NE(registry_.getConnector(name_with_dashes), nullptr);
  EXPECT_NE(registry_.getConnector(name_with_underscores), nullptr);
  EXPECT_NE(registry_.getConnector(name_with_dots), nullptr);

  std::vector<std::string> names = registry_.listConnectors();
  EXPECT_EQ(names.size(), 3);
}

TEST_F(ApiRegistryTest, ListConnectorsAllNamesPresent) {
  std::vector<std::string> original = {"service_alpha", "service_beta", "service_gamma"};

  for (const auto& name : original) {
    registry_.registerConnector(std::make_unique<MockConnector>(name));
  }

  std::vector<std::string> listed = registry_.listConnectors();
  std::sort(listed.begin(), listed.end());
  std::sort(original.begin(), original.end());

  EXPECT_EQ(listed, original);
}

}  // namespace ultra::api
