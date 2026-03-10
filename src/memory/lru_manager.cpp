#include "lru_manager.h"

#include <algorithm>

namespace ultra::memory {

void LruManager::rebuild(const std::vector<ai::FileRecord>& files) {
  order_.clear();
  index_.clear();
  for (auto it = files.rbegin(); it != files.rend(); ++it) {
    order_.push_front(it->path);
    index_[it->path] = order_.begin();
  }
  enforceCap();
}

void LruManager::touch(const std::string& path) {
  auto existingIt = index_.find(path);
  if (existingIt != index_.end()) {
    order_.erase(existingIt->second);
  }
  order_.push_front(path);
  index_[path] = order_.begin();
  enforceCap();
}

void LruManager::erase(const std::string& path) {
  auto it = index_.find(path);
  if (it == index_.end()) {
    return;
  }
  order_.erase(it->second);
  index_.erase(it);
}

std::vector<std::string> LruManager::snapshot() const {
  std::vector<std::string> out;
  out.reserve(order_.size());
  for (const std::string& path : order_) {
    out.push_back(path);
  }
  return out;
}

std::size_t LruManager::size() const noexcept {
  return order_.size();
}

void LruManager::enforceCap() {
  while (order_.size() > kMaxActiveBranches) {
    const std::string leastRecent = order_.back();
    order_.pop_back();
    index_.erase(leastRecent);
  }
}

}  // namespace ultra::memory
