#pragma once

namespace ultra::types {

/// Quantified confidence scores for reasoning outputs.
///
/// All scores are in the range [0.0, 1.0] where 1.0 = maximum confidence.
struct Confidence {
  /// How stable the result is across re-evaluations.
  double stabilityScore{0.0};

  /// Confidence adjusted for known risk factors.
  double riskAdjustedConfidence{0.0};

  /// Overall reliability of the decision.
  double decisionReliabilityIndex{0.0};

  /// Convenience: overall confidence (average of the three scores).
  double overall() const noexcept {
    return (stabilityScore + riskAdjustedConfidence +
            decisionReliabilityIndex) /
           3.0;
  }

  bool operator==(const Confidence& other) const noexcept = default;
};

}  // namespace ultra::types
