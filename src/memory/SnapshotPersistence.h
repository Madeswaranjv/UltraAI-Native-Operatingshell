#pragma once

#include "../ai/RuntimeState.h"
#include "StateGraph.h"
#include "SnapshotChain.h"

#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <string>

namespace ultra::memory {

class SnapshotPersistence {
 public:
  explicit SnapshotPersistence(const std::filesystem::path& baseDir);

  bool saveGraph(uint64_t snapshotId, const StateGraph& graph);
  bool loadGraph(uint64_t snapshotId, StateGraph& graph) const;

  bool saveChain(const SnapshotChain& chain);
  bool loadChain(SnapshotChain& chain) const;
  bool saveRuntimeState(const ai::RuntimeState& state, std::string& error);
  bool loadRuntimeState(ai::RuntimeState& state,
                        std::string& error,
                        std::size_t maxChunks = 0U) const;

 private:
  std::filesystem::path baseDir_;
  std::filesystem::path objectsDir_;
  std::filesystem::path chainFile_;
};

}  // namespace ultra::memory
