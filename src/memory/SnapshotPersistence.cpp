#include "SnapshotPersistence.h"
#include "../core/graph_store/GraphStore.h"
#include "../core/Logger.h"
#include "../runtime/SnapshotSerializer.h"
#include <external/json.hpp>
#include <fstream>

namespace ultra::memory {

SnapshotPersistence::SnapshotPersistence(
    const std::filesystem::path& baseDir)
    : baseDir_(baseDir) {

  objectsDir_ = baseDir_ / "objects";
  chainFile_ = baseDir_ / "chain.json";

  std::error_code ec;
  std::filesystem::create_directories(objectsDir_, ec);
}

bool SnapshotPersistence::saveGraph(
    uint64_t snapshotId,
    const StateGraph& graph) {

  auto file =
      objectsDir_ / (std::to_string(snapshotId) + ".graph");

  StateSnapshot snap = graph.snapshot(snapshotId);
  return runtime::SnapshotSerializer::save(snap, file);
}

bool SnapshotPersistence::loadGraph(
    uint64_t snapshotId,
    StateGraph& graph) const {

  auto file =
      objectsDir_ / (std::to_string(snapshotId) + ".graph");

  StateSnapshot snap;
  if (!runtime::SnapshotSerializer::load(file, snap)) {
    return false;
  }
  if (snap.id != snapshotId) {
    return false;
  }

  graph.restore(snap);

  return true;
}

bool SnapshotPersistence::saveChain(
    const SnapshotChain& chain) {

  nlohmann::json j = nlohmann::json::array();

  for (const auto& s : chain.getHistory()) {

    nlohmann::json item;
    item["id"] = s.id;
    item["hash"] = s.graphHash;
    item["nodes"] = s.nodeCount;
    item["edges"] = s.edgeCount;

    j.push_back(item);
  }

  std::ofstream out(chainFile_);
  if (!out) return false;

  out << j.dump(2);
  return true;
}

bool SnapshotPersistence::loadChain(
    SnapshotChain& chain) const {

  std::ifstream in(chainFile_);
  if (!in) return false;

  nlohmann::json j;

  try {
    in >> j;
    chain.clear();

    for (const auto& item : j) {

      StateSnapshot s;
      s.id = item.value("id", 0ULL);
      s.snapshotId = std::to_string(s.id);
      s.graphHash = item.value("hash", "");
      s.nodeCount = item.value("nodes", 0ULL);
      s.edgeCount = item.value("edges", 0ULL);

      chain.append(s);
    }

  } catch (...) {

    ultra::core::Logger::error(
        ultra::core::LogCategory::General,
        "Failed to parse chain.json");

    return false;
  }

  return true;
}

bool SnapshotPersistence::saveRuntimeState(const ai::RuntimeState& state,
                                           std::string& error) {
  core::graph_store::GraphStore graphStore(baseDir_ / "semantic_graph_store");
  return graphStore.persistFull(state, error);
}

bool SnapshotPersistence::loadRuntimeState(ai::RuntimeState& state,
                                           std::string& error,
                                           const std::size_t maxChunks) const {
  core::graph_store::GraphStore graphStore(baseDir_ / "semantic_graph_store");
  if (maxChunks == 0U) {
    return graphStore.load(state, error);
  }
  return graphStore.loadPartial(maxChunks, state, error);
}

}  // namespace ultra::memory
