// ============================================================================
// File: tests/incremental/test_hashing.cpp
// Tests for Hashing SHA256 computation
// ============================================================================

#include <gtest/gtest.h>
#include <filesystem>
#include "ai/Hashing.h"

using namespace ultra::ai;
namespace fs = std::filesystem;

class HashingTest : public ::testing::Test {
 protected:
  fs::path testDir;

  void SetUp() override {
    testDir = fs::temp_directory_path() / "ultra_test" / "hashing";
    fs::remove_all(testDir);
    fs::create_directories(testDir);
  }

  void TearDown() override {
    fs::remove_all(testDir);
  }
};

// ------------------------------------------------------------
// Basic hashing behavior
// ------------------------------------------------------------

TEST_F(HashingTest, IdenticalInputSameHash) {
  std::string input = "test data";
  auto hash1 = sha256OfString(input);
  auto hash2 = sha256OfString(input);

  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

TEST_F(HashingTest, DifferentInputDifferentHash) {
  auto hash1 = sha256OfString("data1");
  auto hash2 = sha256OfString("data2");

  EXPECT_FALSE(hashesEqual(hash1, hash2));
}

TEST_F(HashingTest, EmptyStringHash) {
  auto hash = sha256OfString("");
  EXPECT_EQ(hash.size(), 32);
}

TEST_F(HashingTest, LargeInputHashing) {
  std::string largeInput(100000, 'a');
  auto hash = sha256OfString(largeInput);
  EXPECT_EQ(hash.size(), 32);
}

// ------------------------------------------------------------
// Byte-level correctness
// ------------------------------------------------------------

TEST_F(HashingTest, StringWithNullTerminator) {
  std::string s1 = "test";
  std::string s2("test\0extra", 10); // include embedded null

  auto hash1 = sha256OfString(s1);
  auto hash2 = sha256OfString(s2);

  EXPECT_FALSE(hashesEqual(hash1, hash2));
}

TEST_F(HashingTest, ByteArrayHashing) {
  std::uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
  auto hash = sha256OfBytes(data, 4);
  EXPECT_EQ(hash.size(), 32);
}

TEST_F(HashingTest, BinaryDataHashing) {
  std::uint8_t binary[] = {0xFF, 0xFE, 0xFD, 0xFC};
  auto hash1 = sha256OfBytes(binary, 4);
  auto hash2 = sha256OfBytes(binary, 4);

  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

TEST_F(HashingTest, EmptyByteArrayHash) {
  auto hash = sha256OfBytes(nullptr, 0);
  EXPECT_EQ(hash.size(), 32);
}

// ------------------------------------------------------------
// Hex encoding
// ------------------------------------------------------------

TEST_F(HashingTest, HashToHexConversion) {
  auto hash = sha256OfString("test");
  auto hex = hashToHex(hash);

  EXPECT_EQ(hex.length(), 64);

  for (char c : hex) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
  }
}

TEST_F(HashingTest, HexConversionLowercase) {
  auto hash = sha256OfString("test");
  auto hex = hashToHex(hash);

  for (char c : hex) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    EXPECT_FALSE(c >= 'A' && c <= 'F');
  }
}

// ------------------------------------------------------------
// Accumulator behavior
// ------------------------------------------------------------

TEST_F(HashingTest, AccumulatorUpdate) {
  Sha256Accumulator acc;
  acc.update("hello");
  acc.update(" ");
  acc.update("world");

  auto hash1 = acc.finalize();
  auto hash2 = sha256OfString("hello world");

  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

TEST_F(HashingTest, PartialByteUpdate) {
  Sha256Accumulator acc1;
  acc1.update("hello");

  Sha256Accumulator acc2;
  std::string hello = "hello";
  acc2.update(reinterpret_cast<const std::uint8_t*>(hello.data()), hello.size());

  auto hash1 = acc1.finalize();
  auto hash2 = acc2.finalize();

  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

TEST_F(HashingTest, AccumulatorFinalizeTwice) {
  Sha256Accumulator acc;
  acc.update("data");

  auto hash1 = acc.finalize();
  auto hash2 = acc.finalize();

  EXPECT_TRUE(hashesEqual(hash1, hash2));
}

// ------------------------------------------------------------
// Determinism and correctness
// ------------------------------------------------------------

TEST_F(HashingTest, DeterministicHashing) {
  std::string input = "deterministic";

  auto h1 = sha256OfString(input);
  auto h2 = sha256OfString(input);
  auto h3 = sha256OfString(input);

  EXPECT_TRUE(hashesEqual(h1, h2));
  EXPECT_TRUE(hashesEqual(h2, h3));
}

TEST_F(HashingTest, ZeroHash) {
  auto zero = zeroHash();

  EXPECT_EQ(zero.size(), 32);
  for (std::uint8_t b : zero) {
    EXPECT_EQ(b, 0);
  }
}

TEST_F(HashingTest, ZeroHashNotEqualToNormalHash) {
  auto normal = sha256OfString("data");
  auto zero = zeroHash();

  EXPECT_FALSE(hashesEqual(normal, zero));
}

TEST_F(HashingTest, CaseSensitiveHashing) {
  auto lower = sha256OfString("data");
  auto upper = sha256OfString("DATA");

  EXPECT_FALSE(hashesEqual(lower, upper));
}

TEST_F(HashingTest, LineEndingConsistency) {
  auto unix = sha256OfString("line1\nline2\nline3");
  auto windows = sha256OfString("line1\r\nline2\r\nline3");

  EXPECT_FALSE(hashesEqual(unix, windows));
}