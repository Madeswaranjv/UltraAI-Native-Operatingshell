#include "BinaryIndexWriter.h"

#include <cstdio>
#include <fstream>

namespace ultra::ai {

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
  for (int i = 0; i < 8; ++i) {
    output.put(static_cast<char>((value >> (static_cast<std::uint64_t>(i) * 8ULL)) &
                                 0xFFULL));
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
    error = std::string("Failed to replace file ") + finalPath.string() +
            ": " + ex.what();
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

bool writeTableHeader(std::ofstream& output,
                      const std::uint32_t magic,
                      const std::uint32_t schemaVersion,
                      const std::uint32_t recordCount) {
  writeUint32(output, magic);
  writeUint32(output, schemaVersion);
  writeUint32(output, recordCount);
  return static_cast<bool>(output);
}

}  // namespace

bool BinaryIndexWriter::writeCoreIndex(const std::filesystem::path& corePath,
                                       const CoreIndex& core,
                                       std::string& error) {
  return writeAtomic(corePath, [&](std::ofstream& output) {
    writeUint32(output, core.magic);
    writeUint32(output, core.indexVersion);
    writeUint32(output, core.schemaVersion);
    writeUint8(output, core.runtimeActive);
    for (const std::uint8_t value : core.reserved) {
      writeUint8(output, value);
    }
    output.write(reinterpret_cast<const char*>(core.filesTblHash.data()),
                 static_cast<std::streamsize>(core.filesTblHash.size()));
    output.write(reinterpret_cast<const char*>(core.symbolsTblHash.data()),
                 static_cast<std::streamsize>(core.symbolsTblHash.size()));
    output.write(reinterpret_cast<const char*>(core.depsTblHash.data()),
                 static_cast<std::streamsize>(core.depsTblHash.size()));
    output.write(reinterpret_cast<const char*>(core.projectRootHash.data()),
                 static_cast<std::streamsize>(core.projectRootHash.size()));
    output.write(reinterpret_cast<const char*>(core.indexHash.data()),
                 static_cast<std::streamsize>(core.indexHash.size()));
    return static_cast<bool>(output);
  }, error);
}

bool BinaryIndexWriter::writeFilesTable(const std::filesystem::path& tablePath,
                                        const std::uint32_t schemaVersion,
                                        const std::vector<FileRecord>& files,
                                        std::string& error) {
  return writeAtomic(tablePath, [&](std::ofstream& output) {
    if (!writeTableHeader(output, kFilesMagic, schemaVersion,
                          static_cast<std::uint32_t>(files.size()))) {
      return false;
    }

    for (const FileRecord& record : files) {
      writeUint32(output, record.fileId);
      writeUint32(output, static_cast<std::uint32_t>(record.path.size()));
      output.write(record.path.data(),
                   static_cast<std::streamsize>(record.path.size()));
      output.write(reinterpret_cast<const char*>(record.hash.data()),
                   static_cast<std::streamsize>(record.hash.size()));
      writeUint8(output, static_cast<std::uint8_t>(record.language));
      // Schema v2+: lastModified moved to files.meta (non-deterministic metadata)
      if (schemaVersion <= 1U) {
        writeUint64(output, record.lastModified);
      }
    }
    return static_cast<bool>(output);
  }, error);
}

bool BinaryIndexWriter::writeSymbolsTable(const std::filesystem::path& tablePath,
                                          const std::uint32_t schemaVersion,
                                          const std::vector<SymbolRecord>& symbols,
                                          std::string& error) {
  return writeAtomic(tablePath, [&](std::ofstream& output) {
    if (!writeTableHeader(output, kSymbolsMagic, schemaVersion,
                          static_cast<std::uint32_t>(symbols.size()))) {
      return false;
    }

    for (const SymbolRecord& symbol : symbols) {
      writeUint64(output, symbol.symbolId);
      writeUint32(output, symbol.fileId);
      writeUint32(output, static_cast<std::uint32_t>(symbol.name.size()));
      output.write(symbol.name.data(),
                   static_cast<std::streamsize>(symbol.name.size()));
      writeUint8(output, static_cast<std::uint8_t>(symbol.symbolType));
      writeUint8(output, static_cast<std::uint8_t>(symbol.visibility));
      writeUint32(output, symbol.lineNumber);
    }
    return static_cast<bool>(output);
  }, error);
}

bool BinaryIndexWriter::writeDependenciesTable(
    const std::filesystem::path& tablePath,
    const std::uint32_t schemaVersion,
    const DependencyTableData& deps,
    std::string& error) {
  return writeAtomic(tablePath, [&](std::ofstream& output) {
    const std::uint32_t totalRecords = static_cast<std::uint32_t>(
        deps.fileEdges.size() + deps.symbolEdges.size());
    if (!writeTableHeader(output, kDepsMagic, schemaVersion, totalRecords)) {
      return false;
    }

    for (const FileDependencyEdge& fileEdge : deps.fileEdges) {
      writeUint8(output, static_cast<std::uint8_t>(DependencyEdgeType::File));
      writeUint64(output, static_cast<std::uint64_t>(fileEdge.fromFileId));
      writeUint64(output, static_cast<std::uint64_t>(fileEdge.toFileId));
    }
    for (const SymbolDependencyEdge& symbolEdge : deps.symbolEdges) {
      writeUint8(output, static_cast<std::uint8_t>(DependencyEdgeType::Symbol));
      writeUint64(output, symbolEdge.fromSymbolId);
      writeUint64(output, symbolEdge.toSymbolId);
    }
    return static_cast<bool>(output);
  }, error);
}

}  // namespace ultra::ai

