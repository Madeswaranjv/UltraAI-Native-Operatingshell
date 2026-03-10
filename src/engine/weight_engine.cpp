#include "weight_engine.h"

namespace ultra::engine {

void WeightEngine::rebuildFromState(const ai::RuntimeState& state) {
  weightsByPath_.clear();
  weightsByPath_.reserve(state.files.size());
  for (const ai::FileRecord& file : state.files) {
    weightsByPath_[file.path] = 1.0;
  }
}

void WeightEngine::incrementalAdd(const std::string& path) {
  weightsByPath_[path] = 1.0;
}

void WeightEngine::incrementalRemove(const std::string& path) {
  weightsByPath_.erase(path);
}

void WeightEngine::incrementalModify(const std::string& path) {
  weightsByPath_[path] = 1.0;
}

double WeightEngine::weightForPath(const std::string& path) const noexcept {
  const auto it = weightsByPath_.find(path);
  if (it == weightsByPath_.end()) {
    return 0.0;
  }
  return it->second;
}

std::size_t WeightEngine::trackedCount() const noexcept {
  return weightsByPath_.size();
}

}  // namespace ultra::engine
