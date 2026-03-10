#include "QueryCache.h"

namespace ultra::engine::query {

QueryCache::QueryCache(const std::size_t capacity) : capacity_(capacity) {}

void QueryCache::clear() {
  entries_.clear();
  indexByKey_.clear();
}

void QueryCache::invalidate(const std::uint64_t version) {
  if (version_ == version) {
    return;
  }
  version_ = version;
  clear();
}

void QueryCache::ensureVersion(const std::uint64_t version) const {
  if (version_ == version) {
    return;
  }
  version_ = version;
  entries_.clear();
  indexByKey_.clear();
}

}  // namespace ultra::engine::query

