#pragma once

#include "../types/Timestamp.h"
#include <string>

namespace ultra::memory {

/// Types of relationships between semantic nodes.
enum class EdgeType {
  Unknown = 0,
  DependsOn = 1,
  Contains = 2,
  Modifies = 3,
  Risks = 4,
  SubtaskOf = 5
};

/// Directed edge representing a relationship in the semantic graph.
struct StateEdge {
  /// Unique identifier for this specific edge instance.
  std::string edgeId;

  /// The node originating the relationship.
  std::string sourceId;

  /// The node receiving the relationship.
  std::string targetId;

  /// Semantic classification of the relationship.
  EdgeType edgeType{EdgeType::Unknown};

  /// Weight indicating relationship strength or importance (0.0 to 1.0).
  double weight{1.0};

  /// Time this edge was established.
  ultra::types::Timestamp timestamp;
  
  bool operator==(const StateEdge& other) const {
    return edgeId == other.edgeId && sourceId == other.sourceId &&
           targetId == other.targetId && edgeType == other.edgeType &&
           weight == other.weight;
  }
};

}  // namespace ultra::memory
