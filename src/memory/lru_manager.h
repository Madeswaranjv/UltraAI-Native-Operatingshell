#pragma once

#include "../ai/FileRegistry.h"

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace ultra::memory {

class LruManager {
 public:
  static constexpr std::size_t kMaxActiveBranches = 3U;

  void rebuild(const std::vector<ai::FileRecord>& files);
  void touch(const std::string& path);
  void erase(const std::string& path);

  [[nodiscard]] std::vector<std::string> snapshot() const;
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  void enforceCap();

  std::list<std::string> order_;
  std::unordered_map<std::string, std::list<std::string>::iterator> index_;
};

}  // namespace ultra::memory
