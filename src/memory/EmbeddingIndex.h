#pragma once

#include <string>
#include <vector>
//E:\Projects\Ultra\src\memory\EmbeddingIndex.h
namespace ultra::memory {

/// Represents the result of a semantic search, binding a node to a similarity score.
struct SearchResult {
  std::string nodeId;
  float similarity{0.0f};
};

/// Interface for generating embeddings from raw text.
class EmbeddingProvider {
 public:
  virtual ~EmbeddingProvider() = default;

  /// Generate a fixed-size float vector embedding for the input text.
  virtual std::vector<float> embed(const std::string& text) = 0;
};

/// Interface for an index capable of semantic similarity search.
class EmbeddingIndex {
 public:
  virtual ~EmbeddingIndex() = default;

  /// Insert an embedding vector associated with a specific memory node.
  virtual void insert(const std::string& nodeId, const std::vector<float>& embedding) = 0;

  /// Find the top K nodes whose embeddings are most similar to the query vector.
  virtual std::vector<SearchResult> search(const std::vector<float>& queryVector, std::size_t topK) const = 0;
  
  /// Remove a node from the index.
  virtual void remove(const std::string& nodeId) = 0;
  
  /// Clear all embeddings.
  virtual void clear() = 0;
};

}  // namespace ultra::memory
