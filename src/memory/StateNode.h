#pragma once

#include "../types/Timestamp.h"
#include <external/json.hpp>

#include <string>

namespace ultra::memory {

/// The type of entities tracked in the memory graph.
enum class NodeType {
  Unknown = 0,
  Task = 1,
  Risk = 2,
  Module = 3,
  Budget = 4,
  Resource = 5,
  File = 6,
  Symbol = 7
};

/// Represents a versioned node in the semantic graph.
struct StateNode {
  /// Unique identifier of the node.
  std::string nodeId;

  /// Semantic type classification.
  NodeType nodeType{NodeType::Unknown};

  /// Structured JSON data payload.
  nlohmann::json data;

  /// Time this node was created/mutated.
  ultra::types::Timestamp timestamp;

  /// Version tracking counter.
  std::uint32_t version{1};
  
  bool operator==(const StateNode& other) const {
    return nodeId == other.nodeId && nodeType == other.nodeType &&
           data == other.data && version == other.version;
  }
};

}  // namespace ultra::memory
