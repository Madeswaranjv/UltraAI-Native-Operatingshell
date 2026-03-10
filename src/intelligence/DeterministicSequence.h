#pragma once
// DeterministicSequence.h

#include <cstdint>

namespace ultra::intelligence {

class DeterministicSequence {
 public:
  DeterministicSequence(uint64_t global = 0ULL, uint64_t id = 0ULL) : globalSequence_(global), idCounter_(id) {}

  // Returns a strictly increasing global sequence value
  uint64_t nextSequence() { return ++globalSequence_; }

  // Returns a strictly increasing id counter used for deterministic id derivation
  uint64_t nextIdCounter() { return ++idCounter_; }

  uint64_t getGlobalSequence() const { return globalSequence_; }
  uint64_t getIdCounter() const { return idCounter_; }

  void setGlobalSequence(uint64_t v) { globalSequence_ = v; }
  void setIdCounter(uint64_t v) { idCounter_ = v; }

 private:
  uint64_t globalSequence_{0};
  uint64_t idCounter_{0};
};

}  // namespace ultra::intelligence
