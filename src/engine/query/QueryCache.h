#pragma once

#include <any>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <utility>

namespace ultra::engine::query {

class QueryCache {
 public:
  explicit QueryCache(std::size_t capacity = 128U);

  void clear();
  void invalidate(std::uint64_t version);

  template <typename ValueT>
  bool get(const std::string& key, std::uint64_t version, ValueT& out) const {
    if (capacity_ == 0U) {
      return false;
    }
    ensureVersion(version);
    const auto it = indexByKey_.find(key);
    if (it == indexByKey_.end()) {
      return false;
    }

    const ValueT* cached = std::any_cast<ValueT>(&it->second->value);
    if (cached == nullptr) {
      return false;
    }

    out = *cached;
    return true;
  }

  template <typename ValueT>
  void put(const std::string& key, std::uint64_t version, ValueT value) {
    if (capacity_ == 0U) {
      return;
    }
    ensureVersion(version);

    const auto existingIt = indexByKey_.find(key);
    if (existingIt != indexByKey_.end()) {
      entries_.erase(existingIt->second);
      indexByKey_.erase(existingIt);
    }

    entries_.push_back(Entry{key, std::any(std::move(value))});
    auto inserted = entries_.end();
    --inserted;
    indexByKey_[key] = inserted;

    if (indexByKey_.size() > capacity_) {
      const auto oldest = entries_.begin();
      indexByKey_.erase(oldest->key);
      entries_.pop_front();
    }
  }

 private:
  struct Entry {
    std::string key;
    std::any value;
  };

  void ensureVersion(std::uint64_t version) const;

  std::size_t capacity_{128U};
  mutable std::uint64_t version_{0U};
  mutable std::list<Entry> entries_;
  mutable std::map<std::string, std::list<Entry>::iterator> indexByKey_;
};

}  // namespace ultra::engine::query

