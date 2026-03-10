#include "EmbeddingIndex.h"
//E:\Projects\Ultra\src\memory\FlatEmbeddingIndex.cpp
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace ultra::memory {

/// A brute-force flat embedding index stub for semantic search.
class FlatEmbeddingIndex : public EmbeddingIndex {
 public:
  void insert(const std::string& nodeId, const std::vector<float>& embedding) override {
    if (embedding.empty()) return;

    if (dimensions_ == 0) {
      dimensions_ = embedding.size();
    } else if (embedding.size() != dimensions_) {
      throw std::invalid_argument("Vector dimension mismatch");
    }

    vectors_[nodeId] = embedding;
  }

  std::vector<SearchResult> search(const std::vector<float>& queryVector, std::size_t topK) const override {
    if (vectors_.empty() || queryVector.size() != dimensions_) {
      return {};
    }

    std::vector<SearchResult> results;
    results.reserve(vectors_.size());

    const float queryNorm = computeNorm(queryVector);

    for (const auto& [nodeId, vec] : vectors_) {
      float dotProduct = 0.0f;
      for (std::size_t i = 0; i < dimensions_; ++i) {
        dotProduct += queryVector[i] * vec[i];
      }
      
      const float vecNorm = computeNorm(vec);
      float similarity = 0.0f;
      
      if (queryNorm > 0.0f && vecNorm > 0.0f) {
        similarity = dotProduct / (queryNorm * vecNorm);
      }
      
      results.push_back({nodeId, similarity});
    }

    // Sort descending by similarity
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
      return a.similarity > b.similarity;
    });

    if (results.size() > topK) {
      results.resize(topK);
    }

    return results;
  }

  void remove(const std::string& nodeId) override {
    vectors_.erase(nodeId);
  }

  void clear() override {
    vectors_.clear();
    dimensions_ = 0;
  }

 private:
  float computeNorm(const std::vector<float>& vec) const {
    float normSquared = 0.0f;
    for (float v : vec) {
      normSquared += v * v;
    }
    return std::sqrt(normSquared);
  }

  std::size_t dimensions_{0};
  std::unordered_map<std::string, std::vector<float>> vectors_;
};

}  // namespace ultra::memory
