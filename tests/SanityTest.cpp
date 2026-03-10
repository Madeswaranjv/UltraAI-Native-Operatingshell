#include <gtest/gtest.h>

// Mock test to allow ultra_tests to compile before other modules are added.
TEST(UltraSanity, BasicMath) {
  EXPECT_EQ(1 + 1, 2);
}
