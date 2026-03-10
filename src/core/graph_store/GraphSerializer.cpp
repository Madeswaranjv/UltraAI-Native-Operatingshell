#include "GraphSerializer.h"

#include <fstream>

namespace ultra::core::graph_store {

namespace {

void writeUint8(std::ofstream& output, const std::uint8_t value) {
  output.put(static_cast<char>(value));
}

void writeUint32(std::ofstream& output, const std::uint32_t value) {
  output.put(static_cast<char>(value & 0xFFU));
  output.put(static_cast<char>((value >> 8U) & 0xFFU));
  output.put(static_cast<char>((value >> 16U) & 0xFFU));
  output.put(static_cast<char>((value >> 24U) & 0xFFU));
}

void writeUint64(std::ofstream& output, const std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    output.put(static_cast<char>((value >> (index * 8)) & 0xFFULL));
  }
}

void writeString(std::ofstream& output, const std::string& value) {
  writeUint32(output, static_cast<std::uint32_t>(value.size()));
  if (!value.empty()) {
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
  }
}

bool replaceFileAtomically(const std::filesystem::path& finalPath,
                           const std::filesystem::path& tempPath,
                           std::string& error) {
  try {
    if (std::filesystem::exists(finalPath)) {
      std::filesystem::remove(finalPath);
    }
    std::filesystem::rename(tempPath, finalPath);
  } catch (const std::exception& ex) {
    error = std::string("Failed to replace file ") + finalPath.string() + ": " +
            ex.what();
    return false;
  }
  return true;
}

template <typename WriteBody>
bool writeAtomic(const std::filesystem::path& path,
                 WriteBody&& body,
                 std::string& error) {
  const std::filesystem::path parent = path.parent_path();
  try {
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
  } catch (const std::exception& ex) {
    error = std::string("Failed to create parent directory: ") + ex.what();
    return false;
  }

  const std::filesystem::path tempPath = path.string() + ".tmp";
  std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Failed to open temp file for writing: " + tempPath.string();
    return false;
  }

  if (!body(output)) {
    if (error.empty()) {
      error = "Failed to write file: " + path.string();
    }
    output.close();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    return false;
  }

  output.flush();
  if (!output) {
    error = "Failed to flush output file: " + tempPath.string();
    output.close();
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    return false;
  }

  output.close();
  if (!replaceFileAtomically(path, tempPath, error)) {
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);
    return false;
  }

  return true;
}

}  // namespace

std::filesystem::path GraphSerializer::chunkPathFor(
    const std::filesystem::path& chunksDir,
    const std::uint32_t chunkId) {
  return chunksDir / ("chunk_" + std::to_string(chunkId) + ".gchk");
}

bool GraphSerializer::writeChunk(const std::filesystem::path& chunksDir,
                                 const GraphChunk& chunk,
                                 std::string& error) {
  const std::filesystem::path path = chunkPathFor(chunksDir, chunk.chunkId);
  return writeAtomic(path, [&](std::ofstream& output) {
    writeUint32(output, kChunkMagic);
    writeUint32(output, kFormatVersion);
    writeUint32(output, chunk.chunkId);
    writeUint32(output, static_cast<std::uint32_t>(chunk.fileNodes.size()));
    writeUint32(output, static_cast<std::uint32_t>(chunk.symbolNodes.size()));
    writeUint32(output, static_cast<std::uint32_t>(chunk.fileEdges.size()));
    writeUint32(output, static_cast<std::uint32_t>(chunk.symbolEdges.size()));
    writeUint32(
        output,
        static_cast<std::uint32_t>(chunk.semanticSymbolDepsByFileId.size()));

    for (const ai::FileRecord& file : chunk.fileNodes) {
      writeUint32(output, file.fileId);
      writeString(output, file.path);
      output.write(reinterpret_cast<const char*>(file.hash.data()),
                   static_cast<std::streamsize>(file.hash.size()));
      writeUint8(output, static_cast<std::uint8_t>(file.language));
      writeUint64(output, file.lastModified);
    }

    for (const ai::SymbolRecord& symbol : chunk.symbolNodes) {
      writeUint64(output, symbol.symbolId);
      writeUint32(output, symbol.fileId);
      writeString(output, symbol.name);
      writeString(output, symbol.signature);
      writeUint8(output, static_cast<std::uint8_t>(symbol.symbolType));
      writeUint8(output, static_cast<std::uint8_t>(symbol.visibility));
      writeUint32(output, symbol.lineNumber);
    }

    for (const ai::FileDependencyEdge& edge : chunk.fileEdges) {
      writeUint32(output, edge.fromFileId);
      writeUint32(output, edge.toFileId);
    }

    for (const ai::SymbolDependencyEdge& edge : chunk.symbolEdges) {
      writeUint64(output, edge.fromSymbolId);
      writeUint64(output, edge.toSymbolId);
    }

    for (const auto& [fileId, dependencies] : chunk.semanticSymbolDepsByFileId) {
      writeUint32(output, fileId);
      writeUint32(output, static_cast<std::uint32_t>(dependencies.size()));
      for (const ai::SemanticSymbolDependency& dependency : dependencies) {
        writeString(output, dependency.fromSymbol);
        writeString(output, dependency.toSymbol);
        writeUint32(output, dependency.lineNumber);
      }
    }

    return static_cast<bool>(output);
  }, error);
}

bool GraphSerializer::writeManifest(const std::filesystem::path& manifestPath,
                                    const std::vector<ChunkManifestEntry>& entries,
                                    std::string& error) {
  return writeAtomic(manifestPath, [&](std::ofstream& output) {
    writeUint32(output, kManifestMagic);
    writeUint32(output, kFormatVersion);
    writeUint32(output, static_cast<std::uint32_t>(entries.size()));
    for (const ChunkManifestEntry& entry : entries) {
      writeUint32(output, entry.chunkId);
      writeUint32(output, entry.fileCount);
      writeUint32(output, entry.symbolCount);
      writeUint32(output, entry.fileEdgeCount);
      writeUint32(output, entry.symbolEdgeCount);
      writeUint32(output, entry.semanticFileCount);
    }
    return static_cast<bool>(output);
  }, error);
}

}  // namespace ultra::core::graph_store

