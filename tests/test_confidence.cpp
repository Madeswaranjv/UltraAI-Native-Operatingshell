// =====================================================
// Confidence Tests
// Tests for Confidence: quantified confidence scores
// =====================================================

#include <gtest/gtest.h>
#include "types/Confidence.h"

namespace ultra::types {

class ConfidenceTest : public ::testing::Test {
 protected:
  Confidence createConfidence() {
    Confidence c;
    c.stabilityScore = 0.8;
    c.riskAdjustedConfidence = 0.75;
    c.decisionReliabilityIndex = 0.85;
    return c;
  }
};

// ===== Default Construction =====

TEST_F(ConfidenceTest, DefaultConstruction) {
  Confidence c;
  EXPECT_EQ(c.stabilityScore, 0.0);
  EXPECT_EQ(c.riskAdjustedConfidence, 0.0);
  EXPECT_EQ(c.decisionReliabilityIndex, 0.0);
}

TEST_F(ConfidenceTest, DefaultOverall) {
  Confidence c;
  EXPECT_EQ(c.overall(), 0.0);
}

// ===== Explicit Assignment =====

TEST_F(ConfidenceTest, ExplicitAssignment) {
  Confidence c = createConfidence();
  EXPECT_EQ(c.stabilityScore, 0.8);
  EXPECT_EQ(c.riskAdjustedConfidence, 0.75);
  EXPECT_EQ(c.decisionReliabilityIndex, 0.85);
}

TEST_F(ConfidenceTest, AssignmentAfterConstruction) {
  Confidence c;
  c.stabilityScore = 0.9;
  c.riskAdjustedConfidence = 0.85;
  c.decisionReliabilityIndex = 0.8;

  EXPECT_EQ(c.stabilityScore, 0.9);
  EXPECT_EQ(c.riskAdjustedConfidence, 0.85);
  EXPECT_EQ(c.decisionReliabilityIndex, 0.8);
}

// ===== Confidence Scoring =====

TEST_F(ConfidenceTest, HighConfidenceCase) {
  Confidence c;
  c.stabilityScore = 0.95;
  c.riskAdjustedConfidence = 0.90;
  c.decisionReliabilityIndex = 0.92;

  double expected = (0.95 + 0.90 + 0.92) / 3.0;
  EXPECT_NEAR(c.overall(), expected, 0.001);
}

TEST_F(ConfidenceTest, LowConfidenceCase) {
  Confidence c;
  c.stabilityScore = 0.2;
  c.riskAdjustedConfidence = 0.15;
  c.decisionReliabilityIndex = 0.25;

  double expected = (0.2 + 0.15 + 0.25) / 3.0;
  EXPECT_NEAR(c.overall(), expected, 0.001);
}

TEST_F(ConfidenceTest, MixedConfidenceCase) {
  Confidence c;
  c.stabilityScore = 0.8;
  c.riskAdjustedConfidence = 0.5;
  c.decisionReliabilityIndex = 0.9;

  double expected = (0.8 + 0.5 + 0.9) / 3.0;
  EXPECT_NEAR(c.overall(), expected, 0.001);
}

// ===== Overall Computation =====

TEST_F(ConfidenceTest, OverallComputation) {
  Confidence c = createConfidence();
  double expected = (0.8 + 0.75 + 0.85) / 3.0;
  EXPECT_NEAR(c.overall(), expected, 0.0001);
}

TEST_F(ConfidenceTest, OverallWithZeros) {
  Confidence c;
  c.stabilityScore = 0.0;
  c.riskAdjustedConfidence = 0.0;
  c.decisionReliabilityIndex = 0.0;

  EXPECT_EQ(c.overall(), 0.0);
}

TEST_F(ConfidenceTest, OverallWithOnes) {
  Confidence c;
  c.stabilityScore = 1.0;
  c.riskAdjustedConfidence = 1.0;
  c.decisionReliabilityIndex = 1.0;

  EXPECT_EQ(c.overall(), 1.0);
}

TEST_F(ConfidenceTest, OverallWithHalfValues) {
  Confidence c;
  c.stabilityScore = 0.5;
  c.riskAdjustedConfidence = 0.5;
  c.decisionReliabilityIndex = 0.5;

  EXPECT_EQ(c.overall(), 0.5);
}

// ===== Score Normalization =====

TEST_F(ConfidenceTest, AllScoresZero) {
  Confidence c;
  EXPECT_EQ(c.overall(), 0.0);
}

TEST_F(ConfidenceTest, AllScoresMax) {
  Confidence c;
  c.stabilityScore = 1.0;
  c.riskAdjustedConfidence = 1.0;
  c.decisionReliabilityIndex = 1.0;

  EXPECT_EQ(c.overall(), 1.0);
}

TEST_F(ConfidenceTest, MixedScoresNormalization) {
  Confidence c;
  c.stabilityScore = 0.3;
  c.riskAdjustedConfidence = 0.6;
  c.decisionReliabilityIndex = 0.9;

  EXPECT_NEAR(c.overall(), 0.6, 0.001);
}

// ===== Negative Value Prevention =====

TEST_F(ConfidenceTest, NoNegativeStabilityScore) {
  Confidence c;
  c.stabilityScore = 0.5;
  EXPECT_GE(c.stabilityScore, 0.0);
}

TEST_F(ConfidenceTest, NoNegativeRiskAdjustedConfidence) {
  Confidence c;
  c.riskAdjustedConfidence = 0.5;
  EXPECT_GE(c.riskAdjustedConfidence, 0.0);
}

TEST_F(ConfidenceTest, NoNegativeDecisionReliabilityIndex) {
  Confidence c;
  c.decisionReliabilityIndex = 0.5;
  EXPECT_GE(c.decisionReliabilityIndex, 0.0);
}

// ===== Overflow Prevention =====

TEST_F(ConfidenceTest, NoOverflowMaxValues) {
  Confidence c;
  c.stabilityScore = 1.0;
  c.riskAdjustedConfidence = 1.0;
  c.decisionReliabilityIndex = 1.0;

  EXPECT_LE(c.overall(), 1.0);
}

TEST_F(ConfidenceTest, HighPrecisionValues) {
  Confidence c;
  c.stabilityScore = 0.333333333;
  c.riskAdjustedConfidence = 0.666666666;
  c.decisionReliabilityIndex = 0.999999999;

  double overall = c.overall();
  EXPECT_GE(overall, 0.0);
  EXPECT_LE(overall, 1.0);
}

// ===== Deterministic Scoring =====

TEST_F(ConfidenceTest, DeterministicOverallComputation) {
  Confidence c = createConfidence();

  double score1 = c.overall();
  double score2 = c.overall();
  double score3 = c.overall();

  EXPECT_EQ(score1, score2);
  EXPECT_EQ(score2, score3);
}

TEST_F(ConfidenceTest, DeterministicAcrossMultipleInstances) {
  Confidence c1 = createConfidence();
  Confidence c2 = createConfidence();

  EXPECT_EQ(c1.overall(), c2.overall());
}

// ===== Stability Across Repeated Calls =====

TEST_F(ConfidenceTest, StabilityMultipleOverallCalls) {
  Confidence c = createConfidence();

  for (int i = 0; i < 10; ++i) {
    double score = c.overall();
    EXPECT_NEAR(score, (0.8 + 0.75 + 0.85) / 3.0, 0.0001);
  }
}

TEST_F(ConfidenceTest, StabilityAfterModification) {
  Confidence c = createConfidence();

  c.stabilityScore = 0.9;
  double score1 = c.overall();
  double score2 = c.overall();

  EXPECT_EQ(score1, score2);
}

// ===== Equality Comparison =====

TEST_F(ConfidenceTest, EqualityIdentical) {
  Confidence c1 = createConfidence();
  Confidence c2 = createConfidence();

  EXPECT_EQ(c1, c2);
}

TEST_F(ConfidenceTest, EqualityDifferentStability) {
  Confidence c1 = createConfidence();
  Confidence c2 = createConfidence();
  c2.stabilityScore = 0.5;

  EXPECT_NE(c1, c2);
}

TEST_F(ConfidenceTest, EqualityDifferentRiskAdjusted) {
  Confidence c1 = createConfidence();
  Confidence c2 = createConfidence();
  c2.riskAdjustedConfidence = 0.5;

  EXPECT_NE(c1, c2);
}

TEST_F(ConfidenceTest, EqualityDifferentDecisionReliability) {
  Confidence c1 = createConfidence();
  Confidence c2 = createConfidence();
  c2.decisionReliabilityIndex = 0.5;

  EXPECT_NE(c1, c2);
}

TEST_F(ConfidenceTest, EqualityWithZeros) {
  Confidence c1;
  Confidence c2;

  EXPECT_EQ(c1, c2);
}

TEST_F(ConfidenceTest, EqualityWithMaxValues) {
  Confidence c1;
  c1.stabilityScore = 1.0;
  c1.riskAdjustedConfidence = 1.0;
  c1.decisionReliabilityIndex = 1.0;

  Confidence c2;
  c2.stabilityScore = 1.0;
  c2.riskAdjustedConfidence = 1.0;
  c2.decisionReliabilityIndex = 1.0;

  EXPECT_EQ(c1, c2);
}

// ===== Boundary Values =====

TEST_F(ConfidenceTest, BoundaryZeroScore) {
  Confidence c;
  c.stabilityScore = 0.0;

  EXPECT_EQ(c.stabilityScore, 0.0);
}

TEST_F(ConfidenceTest, BoundaryMaxScore) {
  Confidence c;
  c.stabilityScore = 1.0;

  EXPECT_EQ(c.stabilityScore, 1.0);
}

TEST_F(ConfidenceTest, BoundaryVerySmallScore) {
  Confidence c;
  c.stabilityScore = 0.0001;

  EXPECT_NEAR(c.stabilityScore, 0.0001, 0.00001);
}

TEST_F(ConfidenceTest, BoundaryVeryHighScore) {
  Confidence c;
  c.stabilityScore = 0.9999;

  EXPECT_NEAR(c.stabilityScore, 0.9999, 0.00001);
}

// ===== Edge Cases =====

TEST_F(ConfidenceTest, ZeroConfidenceAllScores) {
  Confidence c;
  c.stabilityScore = 0.0;
  c.riskAdjustedConfidence = 0.0;
  c.decisionReliabilityIndex = 0.0;

  EXPECT_EQ(c.overall(), 0.0);
}

TEST_F(ConfidenceTest, MaxConfidenceAllScores) {
  Confidence c;
  c.stabilityScore = 1.0;
  c.riskAdjustedConfidence = 1.0;
  c.decisionReliabilityIndex = 1.0;

  EXPECT_EQ(c.overall(), 1.0);
}

TEST_F(ConfidenceTest, AsymmetricScores) {
  Confidence c;
  c.stabilityScore = 1.0;
  c.riskAdjustedConfidence = 0.0;
  c.decisionReliabilityIndex = 0.5;

  double expected = (1.0 + 0.0 + 0.5) / 3.0;
  EXPECT_NEAR(c.overall(), expected, 0.001);
}

TEST_F(ConfidenceTest, PrecisionRounding) {
  Confidence c;
  c.stabilityScore = 0.123456789;
  c.riskAdjustedConfidence = 0.987654321;
  c.decisionReliabilityIndex = 0.555555555;

  double overall = c.overall();
  EXPECT_GE(overall, 0.0);
  EXPECT_LE(overall, 1.0);
}

// ===== Large Batch Scoring =====

TEST_F(ConfidenceTest, LargeBatchConfidenceScoring) {
  std::vector<Confidence> confidences;

  for (int i = 0; i < 30; ++i) {
    Confidence c;
    c.stabilityScore = 0.5 + (i % 5) * 0.1;
    c.riskAdjustedConfidence = 0.4 + (i % 7) * 0.08;
    c.decisionReliabilityIndex = 0.6 + (i % 3) * 0.12;
    confidences.push_back(c);
  }

  EXPECT_EQ(confidences.size(), 30);

  for (int i = 0; i < 30; ++i) {
    double score = confidences[i].overall();
    EXPECT_GE(score, 0.0);
    EXPECT_LE(score, 1.0);
  }
}

// ===== Field Access Patterns =====

TEST_F(ConfidenceTest, IndependentFieldModification) {
  Confidence c;

  c.stabilityScore = 0.5;
  EXPECT_EQ(c.stabilityScore, 0.5);
  EXPECT_EQ(c.riskAdjustedConfidence, 0.0);

  c.riskAdjustedConfidence = 0.7;
  EXPECT_EQ(c.stabilityScore, 0.5);
  EXPECT_EQ(c.riskAdjustedConfidence, 0.7);

  c.decisionReliabilityIndex = 0.9;
  EXPECT_EQ(c.decisionReliabilityIndex, 0.9);
}

TEST_F(ConfidenceTest, FieldReadAfterWrite) {
  Confidence c;

  c.stabilityScore = 0.8;
  EXPECT_EQ(c.stabilityScore, 0.8);

  c.stabilityScore = 0.9;
  EXPECT_EQ(c.stabilityScore, 0.9);
}

}  // namespace ultra::types
