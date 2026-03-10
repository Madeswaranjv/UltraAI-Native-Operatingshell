#include "SnapshotSerializer.h"

#include "../types/Timestamp.h"

#include <external/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <type_traits>
#include <vector>

namespace ultra::runtime {

namespace {

template <typename T>
bool writePod(std::ostream& output, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>,
                "writePod requires trivially copyable input");
  std::array<char, sizeof(T)> buffer{};
  std::memcpy(buffer.data(), &value, sizeof(T));
  output.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  return static_cast<bool>(output);
}

template <typename T>
bool readPod(std::istream& input, T& valueOut) {
  static_assert(std::is_trivially_copyable_v<T>,
                "readPod requires trivially copyable input");
  std::array<char, sizeof(T)> buffer{};
  input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  if (!input) {
    return false;
  }
  std::memcpy(&valueOut, buffer.data(), sizeof(T));
  return true;
}

bool lessNodeById(const memory::StateNode& left, const memory::StateNode& right) {
  return left.nodeId < right.nodeId;
}

bool lessEdgeByEndpoints(const memory::StateEdge& left,
                         const memory::StateEdge& right) {
  if (left.sourceId != right.sourceId) {
    return left.sourceId < right.sourceId;
  }
  if (left.targetId != right.targetId) {
    return left.targetId < right.targetId;
  }
  if (left.edgeType != right.edgeType) {
    return static_cast<std::uint32_t>(left.edgeType) <
           static_cast<std::uint32_t>(right.edgeType);
  }
  if (left.edgeId != right.edgeId) {
    return left.edgeId < right.edgeId;
  }
  if (left.weight != right.weight) {
    return left.weight < right.weight;
  }
  return left.timestamp.epochMs() < right.timestamp.epochMs();
}

}  // namespace

bool SnapshotSerializer::save(const memory::StateSnapshot& snapshot,
                              const std::filesystem::path& path) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }

  const SnapshotHeader header{.formatVersion = kFormatVersion,
                              .snapshotVersion = snapshot.id};
  if (!writePod(output, header.formatVersion) ||
      !writePod(output, header.snapshotVersion)) {
    return false;
  }

  if (!writeString(output, snapshot.snapshotId) ||
      !writeString(output, snapshot.graphHash)) {
    return false;
  }

  std::vector<memory::StateNode> nodes = snapshot.nodes;
  std::sort(nodes.begin(), nodes.end(), lessNodeById);

  const std::uint64_t nodeCount = static_cast<std::uint64_t>(nodes.size());
  if (!writePod(output, nodeCount)) {
    return false;
  }

  for (const memory::StateNode& node : nodes) {
    const std::uint32_t nodeType = static_cast<std::uint32_t>(node.nodeType);
    const std::int64_t timestampMs = node.timestamp.epochMs();
    const std::string metadata = node.data.dump();

    if (!writeString(output, node.nodeId) ||
        !writePod(output, nodeType) ||
        !writePod(output, node.version) ||
        !writePod(output, timestampMs) ||
        !writeString(output, metadata)) {
      return false;
    }
  }

  std::vector<memory::StateEdge> edges = snapshot.edges;
  std::sort(edges.begin(), edges.end(), lessEdgeByEndpoints);

  const std::uint64_t edgeCount = static_cast<std::uint64_t>(edges.size());
  if (!writePod(output, edgeCount)) {
    return false;
  }

  for (const memory::StateEdge& edge : edges) {
    const std::uint32_t edgeType = static_cast<std::uint32_t>(edge.edgeType);
    const std::int64_t timestampMs = edge.timestamp.epochMs();
    if (!writeString(output, edge.edgeId) ||
        !writeString(output, edge.sourceId) ||
        !writeString(output, edge.targetId) ||
        !writePod(output, edgeType) ||
        !writePod(output, edge.weight) ||
        !writePod(output, timestampMs)) {
      return false;
    }
  }

  return static_cast<bool>(output);
}

bool SnapshotSerializer::load(const std::filesystem::path& path,
                              memory::StateSnapshot& snapshotOut) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return false;
  }

  SnapshotHeader header{};
  if (!readPod(input, header.formatVersion) ||
      !readPod(input, header.snapshotVersion)) {
    return false;
  }
  if (header.formatVersion != kFormatVersion) {
    return false;
  }

  std::string snapshotId;
  std::string graphHash;
  if (!readString(input, snapshotId) ||
      !readString(input, graphHash)) {
    return false;
  }

  std::uint64_t nodeCount = 0ULL;
  if (!readPod(input, nodeCount)) {
    return false;
  }
  if (nodeCount >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }

  std::vector<memory::StateNode> nodes;
  nodes.reserve(static_cast<std::size_t>(nodeCount));
  for (std::uint64_t index = 0ULL; index < nodeCount; ++index) {
    memory::StateNode node;
    std::uint32_t nodeType = 0U;
    std::int64_t timestampMs = 0LL;
    std::string metadata;

    if (!readString(input, node.nodeId) ||
        !readPod(input, nodeType) ||
        !readPod(input, node.version) ||
        !readPod(input, timestampMs) ||
        !readString(input, metadata)) {
      return false;
    }

    node.nodeType = static_cast<memory::NodeType>(nodeType);
    node.timestamp = ultra::types::Timestamp::fromEpochMs(timestampMs);
    node.data = nlohmann::json::parse(metadata, nullptr, false);
    if (node.data.is_discarded()) {
      return false;
    }
    nodes.push_back(std::move(node));
  }

  std::uint64_t edgeCount = 0ULL;
  if (!readPod(input, edgeCount)) {
    return false;
  }
  if (edgeCount >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }

  std::vector<memory::StateEdge> edges;
  edges.reserve(static_cast<std::size_t>(edgeCount));
  for (std::uint64_t index = 0ULL; index < edgeCount; ++index) {
    memory::StateEdge edge;
    std::uint32_t edgeType = 0U;
    std::int64_t timestampMs = 0LL;

    if (!readString(input, edge.edgeId) ||
        !readString(input, edge.sourceId) ||
        !readString(input, edge.targetId) ||
        !readPod(input, edgeType) ||
        !readPod(input, edge.weight) ||
        !readPod(input, timestampMs)) {
      return false;
    }

    edge.edgeType = static_cast<memory::EdgeType>(edgeType);
    edge.timestamp = ultra::types::Timestamp::fromEpochMs(timestampMs);
    edges.push_back(std::move(edge));
  }

  snapshotOut = memory::StateSnapshot{};
  snapshotOut.id = header.snapshotVersion;
  snapshotOut.snapshotId =
      snapshotId.empty() ? std::to_string(header.snapshotVersion) : snapshotId;
  snapshotOut.graphHash = std::move(graphHash);
  snapshotOut.nodes = std::move(nodes);
  snapshotOut.edges = std::move(edges);
  snapshotOut.nodeCount = snapshotOut.nodes.size();
  snapshotOut.edgeCount = snapshotOut.edges.size();
  return true;
}

bool SnapshotSerializer::writeString(std::ostream& output,
                                     const std::string& value) {
  const std::uint64_t length = static_cast<std::uint64_t>(value.size());
  if (!writePod(output, length)) {
    return false;
  }
  output.write(value.data(), static_cast<std::streamsize>(length));
  return static_cast<bool>(output);
}

bool SnapshotSerializer::readString(std::istream& input,
                                    std::string& valueOut) {
  std::uint64_t length = 0ULL;
  if (!readPod(input, length)) {
    return false;
  }
  if (length >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  valueOut.assign(static_cast<std::size_t>(length), '\0');
  if (length == 0ULL) {
    return true;
  }
  input.read(valueOut.data(), static_cast<std::streamsize>(length));
  return static_cast<bool>(input);
}

}  // namespace ultra::runtime

