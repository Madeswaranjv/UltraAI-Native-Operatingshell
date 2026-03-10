// =====================================================
// Result Tests
// Tests for Result<T, E> generic result type
// =====================================================

#include <gtest/gtest.h>
#include "types/Result.h"

#include <string>
#include <vector>

namespace ultra::types {

class ResultTest : public ::testing::Test {
 protected:
};

// ===== Success Result Construction =====

TEST_F(ResultTest, CreateOkResult) {
  Result<int, std::string> r = Result<int, std::string>::ok(42);
  EXPECT_TRUE(r.isOk());
  EXPECT_FALSE(r.isErr());
  EXPECT_EQ(r.value(), 42);
}

TEST_F(ResultTest, CreateOkWithString) {
  Result<std::string, int> r = Result<std::string, int>::ok("success");
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value(), "success");
}

TEST_F(ResultTest, CreateOkWithVector) {
  std::vector<int> vec = {1, 2, 3};
  Result<std::vector<int>, std::string> r = Result<std::vector<int>, std::string>::ok(vec);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value().size(), 3);
  EXPECT_EQ(r.value()[0], 1);
}

// ===== Error Result Construction =====

TEST_F(ResultTest, CreateErrResult) {
  Result<int, std::string> r = Result<int, std::string>::err("error message");
  EXPECT_FALSE(r.isOk());
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.error(), "error message");
}

TEST_F(ResultTest, CreateErrWithInt) {
  Result<std::string, int> r = Result<std::string, int>::err(404);
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.error(), 404);
}

TEST_F(ResultTest, CreateErrWithCustomType) {
  struct CustomError {
    int code;
    std::string msg;
  };

  Result<int, CustomError> r = Result<int, CustomError>::err(CustomError{500, "Internal error"});
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.error().code, 500);
  EXPECT_EQ(r.error().msg, "Internal error");
}

// ===== Value/Error Access =====

TEST_F(ResultTest, AccessOkValue) {
  Result<int, std::string> r = Result<int, std::string>::ok(100);
  EXPECT_EQ(r.value(), 100);
}

TEST_F(ResultTest, AccessErrValue) {
  Result<int, std::string> r = Result<int, std::string>::err("error");
  EXPECT_EQ(r.error(), "error");
}

TEST_F(ResultTest, ValueOrOnOk) {
  Result<int, std::string> r = Result<int, std::string>::ok(42);
  EXPECT_EQ(r.valueOr(999), 42);
}

TEST_F(ResultTest, ValueOrOnErr) {
  Result<int, std::string> r = Result<int, std::string>::err("error");
  EXPECT_EQ(r.valueOr(999), 999);
}

TEST_F(ResultTest, ValueOrWithString) {
  Result<std::string, int> r = Result<std::string, int>::err(404);
  EXPECT_EQ(r.valueOr("default"), "default");
}

// ===== Multiple Value Types =====

TEST_F(ResultTest, ResultWithBool) {
  Result<bool, std::string> r1 = Result<bool, std::string>::ok(true);
  Result<bool, std::string> r2 = Result<bool, std::string>::ok(false);

  EXPECT_TRUE(r1.isOk());
  EXPECT_TRUE(r1.value());
  EXPECT_TRUE(r2.isOk());
  EXPECT_FALSE(r2.value());
}

TEST_F(ResultTest, ResultWithDouble) {
  Result<double, std::string> r = Result<double, std::string>::ok(3.14159);
  EXPECT_TRUE(r.isOk());
  EXPECT_NEAR(r.value(), 3.14159, 0.00001);
}

TEST_F(ResultTest, ResultWithLongString) {
  std::string longStr;
  for (int i = 0; i < 100; ++i) {
    longStr += "segment ";
  }

  Result<std::string, int> r = Result<std::string, int>::ok(longStr);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value().length(), longStr.length());
}

// ===== Multiple Error Types =====

TEST_F(ResultTest, ResultWithIntError) {
  Result<std::string, int> r = Result<std::string, int>::err(404);
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.error(), 404);
}

TEST_F(ResultTest, ResultWithStructError) {
  struct ErrorInfo {
    int code;
    std::string details;
    bool operator==(const ErrorInfo& other) const {
      return code == other.code && details == other.details;
    }
  };

  ErrorInfo err{500, "Server error"};
  Result<int, ErrorInfo> r = Result<int, ErrorInfo>::err(err);
  EXPECT_TRUE(r.isErr());
  EXPECT_EQ(r.error().code, 500);
}

// ===== Invalid State Handling =====

TEST_F(ResultTest, OkCheckOnOk) {
  Result<int, std::string> r = Result<int, std::string>::ok(42);
  EXPECT_TRUE(r.isOk());
}

TEST_F(ResultTest, OkCheckOnErr) {
  Result<int, std::string> r = Result<int, std::string>::err("error");
  EXPECT_FALSE(r.isOk());
}

TEST_F(ResultTest, ErrCheckOnOk) {
  Result<int, std::string> r = Result<int, std::string>::ok(42);
  EXPECT_FALSE(r.isErr());
}

TEST_F(ResultTest, ErrCheckOnErr) {
  Result<int, std::string> r = Result<int, std::string>::err("error");
  EXPECT_TRUE(r.isErr());
}

// ===== Value Retrieval from Rvalue References =====

TEST_F(ResultTest, MoveValue) {
  Result<std::string, int> r = Result<std::string, int>::ok("test");
  std::string val = std::move(r).value();
  EXPECT_EQ(val, "test");
}

TEST_F(ResultTest, MoveError) {
  Result<int, std::string> r = Result<int, std::string>::err("error");
  std::string err = std::move(r).error();
  EXPECT_EQ(err, "error");
}

// ===== Large Payloads =====

TEST_F(ResultTest, LargeVectorPayload) {
  std::vector<int> large_vec;
  for (int i = 0; i < 1000; ++i) {
    large_vec.push_back(i);
  }

  Result<std::vector<int>, std::string> r = Result<std::vector<int>, std::string>::ok(large_vec);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value().size(), 1000);
  EXPECT_EQ(r.value()[500], 500);
}

TEST_F(ResultTest, LargeStringPayload) {
  std::string large_str;
  for (int i = 0; i < 5000; ++i) {
    large_str += "x";
  }

  Result<std::string, int> r = Result<std::string, int>::ok(large_str);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value().length(), 5000);
}

// ===== Deterministic Behavior =====

TEST_F(ResultTest, DeterministicOk) {
  for (int i = 0; i < 10; ++i) {
    Result<int, std::string> r = Result<int, std::string>::ok(42);
    EXPECT_TRUE(r.isOk());
    EXPECT_EQ(r.value(), 42);
  }
}

TEST_F(ResultTest, DeterministicErr) {
  for (int i = 0; i < 10; ++i) {
    Result<int, std::string> r = Result<int, std::string>::err("error");
    EXPECT_TRUE(r.isErr());
    EXPECT_EQ(r.error(), "error");
  }
}

TEST_F(ResultTest, DeterministicValueOr) {
  Result<int, std::string> r_ok = Result<int, std::string>::ok(42);
  Result<int, std::string> r_err = Result<int, std::string>::err("error");

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(r_ok.valueOr(999), 42);
    EXPECT_EQ(r_err.valueOr(999), 999);
  }
}

// ===== Stability Across Repeated Operations =====

TEST_F(ResultTest, StabilityMultipleAccess) {
  Result<std::string, int> r = Result<std::string, int>::ok("value");

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(r.value(), "value");
    EXPECT_TRUE(r.isOk());
  }
}

TEST_F(ResultTest, StabilityErrAccess) {
  Result<int, std::string> r = Result<int, std::string>::err("error");

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(r.error(), "error");
    EXPECT_TRUE(r.isErr());
  }
}

// ===== Edge Cases =====

TEST_F(ResultTest, OkWithZero) {
  Result<int, std::string> r = Result<int, std::string>::ok(0);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value(), 0);
}

TEST_F(ResultTest, OkWithNegativeNumber) {
  Result<int, std::string> r = Result<int, std::string>::ok(-42);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value(), -42);
}

TEST_F(ResultTest, OkWithEmptyString) {
  Result<std::string, int> r = Result<std::string, int>::ok("");
  EXPECT_TRUE(r.isOk());
  EXPECT_TRUE(r.value().empty());
}

TEST_F(ResultTest, ErrWithEmptyString) {
  Result<int, std::string> r = Result<int, std::string>::err("");
  EXPECT_TRUE(r.isErr());
  EXPECT_TRUE(r.error().empty());
}

TEST_F(ResultTest, OkWithEmptyVector) {
  std::vector<int> empty_vec;
  Result<std::vector<int>, std::string> r = Result<std::vector<int>, std::string>::ok(empty_vec);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value().size(), 0);
}

TEST_F(ResultTest, OkWithMaxInt) {
  Result<int, std::string> r = Result<int, std::string>::ok(INT_MAX);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value(), INT_MAX);
}

TEST_F(ResultTest, OkWithMinInt) {
  Result<int, std::string> r = Result<int, std::string>::ok(INT_MIN);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value(), INT_MIN);
}

TEST_F(ResultTest, OkWithZeroDouble) {
  Result<double, std::string> r = Result<double, std::string>::ok(0.0);
  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value(), 0.0);
}

TEST_F(ResultTest, OkWithNegativeDouble) {
  Result<double, std::string> r = Result<double, std::string>::ok(-3.14159);
  EXPECT_TRUE(r.isOk());
  EXPECT_NEAR(r.value(), -3.14159, 0.00001);
}

// ===== Chaining Results =====

TEST_F(ResultTest, ChainOkResults) {
  Result<int, std::string> r1 = Result<int, std::string>::ok(10);
  Result<int, std::string> r2 = Result<int, std::string>::ok(20);
  Result<int, std::string> r3 = Result<int, std::string>::ok(30);

  if (r1.isOk() && r2.isOk() && r3.isOk()) {
    int sum = r1.value() + r2.value() + r3.value();
    EXPECT_EQ(sum, 60);
  }
}

TEST_F(ResultTest, ChainWithError) {
  Result<int, std::string> r1 = Result<int, std::string>::ok(10);
  Result<int, std::string> r2 = Result<int, std::string>::err("error");
  Result<int, std::string> r3 = Result<int, std::string>::ok(30);

  EXPECT_TRUE(r1.isOk());
  EXPECT_TRUE(r2.isErr());
  EXPECT_TRUE(r3.isOk());
  EXPECT_EQ(r2.error(), "error");
}

// ===== Type Diversity =====

TEST_F(ResultTest, ResultWithComplexTypes) {
  struct Config {
    std::string name;
    int version;
    bool enabled;
  };

  Config cfg = {"MyConfig", 1, true};
  Result<Config, std::string> r = Result<Config, std::string>::ok(cfg);

  EXPECT_TRUE(r.isOk());
  EXPECT_EQ(r.value().name, "MyConfig");
  EXPECT_EQ(r.value().version, 1);
  EXPECT_TRUE(r.value().enabled);
}

}  // namespace ultra::types
