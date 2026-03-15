#include "QueryCache.h"

namespace ultra::engine::query {

QueryCache::QueryCache(const std::size_t capacity) : capacity_(capacity) {}

void QueryCache::clear() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  entries_.clear();
  indexByKey_.clear();
}

void QueryCache::invalidate(const std::uint64_t version) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (version_ == version) {
    return;
  }
  version_ = version;
  entries_.clear();
  indexByKey_.clear();
}

void QueryCache::ensureVersion(const std::uint64_t version) const {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (version_ == version) {
    return;
  }
  version_ = version;
  entries_.clear();
  indexByKey_.clear();
}

}  // namespace ultra::engine::query
