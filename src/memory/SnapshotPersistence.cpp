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
  std::filesystem::create_directories(baseDir_, ec);
  std::filesystem::create_directories(objectsDir_, ec);
}

std::filesystem::path SnapshotPersistence::graphPath(
    const uint64_t snapshotId) const {
  return objectsDir_ / (std::to_string(snapshotId) + ".graph");
}

bool SnapshotPersistence::saveGraph(
    uint64_t snapshotId,
    const StateGraph& graph) {
  StateSnapshot snap = graph.snapshot(snapshotId);
  return saveGraph(snap);
}

bool SnapshotPersistence::saveGraph(const StateSnapshot& snapshot) {
  if (snapshot.id == 0ULL) {
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(objectsDir_, ec);

  StateSnapshot normalized = snapshot;
  if (normalized.snapshotId.empty()) {
    normalized.snapshotId = std::to_string(normalized.id);
  }
  if (normalized.createdAt.epochMs() == 0LL) {
    normalized.createdAt = ultra::types::Timestamp::now();
  }

  return runtime::SnapshotSerializer::save(normalized, graphPath(normalized.id));
}

bool SnapshotPersistence::loadGraph(
    uint64_t snapshotId,
    StateGraph& graph) const {
  StateSnapshot snap;
  if (!loadSnapshot(snapshotId, snap)) {
    return false;
  }

  graph = StateGraph::fromSnapshot(snap);

  return true;
}

bool SnapshotPersistence::loadSnapshot(uint64_t snapshotId,
                                       StateSnapshot& snapshotOut) const {
  if (!runtime::SnapshotSerializer::load(graphPath(snapshotId), snapshotOut)) {
    return false;
  }
  if (snapshotOut.id != snapshotId) {
    return false;
  }
  if (snapshotOut.snapshotId.empty()) {
    snapshotOut.snapshotId = std::to_string(snapshotId);
  }
  return true;
}

bool SnapshotPersistence::hasGraph(uint64_t snapshotId) const {
  std::error_code ec;
  return std::filesystem::exists(graphPath(snapshotId), ec);
}

bool SnapshotPersistence::saveChain(
    const SnapshotChain& chain) {
  std::error_code ec;
  std::filesystem::create_directories(baseDir_, ec);

  nlohmann::json j = nlohmann::json::array();

  for (const auto& s : chain.getHistory()) {
    StateSnapshot normalized = s;
    if (normalized.snapshotId.empty()) {
      normalized.snapshotId = std::to_string(normalized.id);
    }

    nlohmann::json item;
    item["id"] = normalized.id;
    item["snapshot_id"] = normalized.snapshotId;
    item["branch_id"] = normalized.branchId;
    item["timestamp_ms"] = normalized.createdAt.epochMs();
    item["hash"] = normalized.graphHash;
    item["nodes"] = normalized.nodeCount;
    item["edges"] = normalized.edgeCount;

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
      const uint64_t snapshotId = item.value("id", 0ULL);
      if (snapshotId == 0ULL) {
        continue;
      }

      StateSnapshot s;
      if (!loadSnapshot(snapshotId, s)) {
        ultra::core::Logger::warning(
            ultra::core::LogCategory::General,
            "Skipping missing snapshot object " + std::to_string(snapshotId));
        continue;
      }
      s.snapshotId = item.value("snapshot_id", s.snapshotId);
      s.branchId = item.value("branch_id", s.branchId);
      s.createdAt = ultra::types::Timestamp::fromEpochMs(
          item.value("timestamp_ms", s.createdAt.epochMs()));
      s.graphHash = item.value("hash", s.graphHash);
      s.nodeCount = item.value("nodes", s.nodeCount);
      s.edgeCount = item.value("edges", s.edgeCount);

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
