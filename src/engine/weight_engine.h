#pragma once

#include "../ai/RuntimeState.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace ultra::engine {

class WeightEngine {
 public:
  void rebuildFromState(const ai::RuntimeState& state);
  void incrementalAdd(const std::string& path);
  void incrementalRemove(const std::string& path);
  void incrementalModify(const std::string& path);

  [[nodiscard]] double weightForPath(const std::string& path) const noexcept;
  [[nodiscard]] std::size_t trackedCount() const noexcept;

 private:
  std::unordered_map<std::string, double> weightsByPath_;
};

}  // namespace ultra::engine
