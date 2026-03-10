#include "BinaryIndexReader.h"

#include "BinaryIndexWriter.h"

#include <array>
#include <fstream>
#include <map>

namespace ultra::ai {

namespace {

bool readUint8(std::ifstream& input, std::uint8_t& value) {
  char ch = 0;
  if (!input.get(ch)) {
    return false;
  }
  value = static_cast<std::uint8_t>(ch);
  return true;
}

bool readUint32(std::ifstream& input, std::uint32_t& value) {
  std::array<std::uint8_t, 4> bytes{};
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  value = static_cast<std::uint32_t>(bytes[0]) |
          (static_cast<std::uint32_t>(bytes[1]) << 8U) |
          (static_cast<std::uint32_t>(bytes[2]) << 16U) |
          (static_cast<std::uint32_t>(bytes[3]) << 24U);
  return true;
}

bool readUint64(std::ifstream& input, std::uint64_t& value) {
  std::array<std::uint8_t, 8> bytes{};
  if (!input.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
    return false;
  }
  value = 0ULL;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(i)])
             << (static_cast<std::uint64_t>(i) * 8ULL);
  }
  return true;
}

bool readTableHeader(std::ifstream& input,
                     const std::uint32_t expectedMagic,
                     const std::uint32_t expectedSchema,
                     std::uint32_t& outRecordCount,
                     std::string& error) {
  std::uint32_t magic = 0;
  std::uint32_t schema = 0;
  std::uint32_t recordCount = 0;
  if (!readUint32(input, magic) || !readUint32(input, schema) ||
      !readUint32(input, recordCount)) {
    error = "Failed to read table header";
    return false;
  }
  if (magic != expectedMagic) {
    error = "Table magic mismatch";
    return false;
  }
  if (schema != expectedSchema) {
    error = "Table schema version mismatch";
    return false;
  }
  outRecordCount = recordCount;
  return true;
}

bool readString(std::ifstream& input, std::uint32_t length, std::string& value) {
  value.assign(length, '\0');
  if (length == 0U) {
    return true;
  }
  return static_cast<bool>(
      input.read(value.data(), static_cast<std::streamsize>(length)));
}

}  // namespace

bool BinaryIndexReader::readCoreIndex(const std::filesystem::path& corePath,
                                      CoreIndex& outCore,
                                      std::string& error) {
  std::ifstream input(corePath, std::ios::binary);
  if (!input) {
    error = "Failed to open core.idx: " + corePath.string();
    return false;
  }

  if (!readUint32(input, outCore.magic) ||
      !readUint32(input, outCore.indexVersion) ||
      !readUint32(input, outCore.schemaVersion)) {
    error = "Failed to read core.idx header";
    return false;
  }

  if (!readUint8(input, outCore.runtimeActive)) {
    error = "Failed to read core.idx runtime_active";
    return false;
  }

  for (std::uint8_t& value : outCore.reserved) {
    if (!readUint8(input, value)) {
      error = "Failed to read core.idx reserved bytes";
      return false;
    }
  }

  if (!input.read(reinterpret_cast<char*>(outCore.filesTblHash.data()),
                  static_cast<std::streamsize>(outCore.filesTblHash.size())) ||
      !input.read(reinterpret_cast<char*>(outCore.symbolsTblHash.data()),
                  static_cast<std::streamsize>(outCore.symbolsTblHash.size())) ||
      !input.read(reinterpret_cast<char*>(outCore.depsTblHash.data()),
                  static_cast<std::streamsize>(outCore.depsTblHash.size())) ||
      !input.read(reinterpret_cast<char*>(outCore.projectRootHash.data()),
                  static_cast<std::streamsize>(outCore.projectRootHash.size())) ||
      !input.read(reinterpret_cast<char*>(outCore.indexHash.data()),
                  static_cast<std::streamsize>(outCore.indexHash.size()))) {
    error = "Failed to read core.idx hash payload";
    return false;
  }

  return true;
}

bool BinaryIndexReader::readFilesTable(const std::filesystem::path& tablePath,
                                       const std::uint32_t expectedSchemaVersion,
                                       std::vector<FileRecord>& outFiles,
                                       std::string& error) {
  outFiles.clear();
  std::ifstream input(tablePath, std::ios::binary);
  if (!input) {
    error = "Failed to open files.tbl: " + tablePath.string();
    return false;
  }

  std::uint32_t recordCount = 0;
  if (!readTableHeader(input, BinaryIndexWriter::kFilesMagic, expectedSchemaVersion,
                       recordCount, error)) {
    return false;
  }

  outFiles.reserve(recordCount);
  for (std::uint32_t i = 0; i < recordCount; ++i) {
    FileRecord record;
    std::uint32_t pathLength = 0;
    std::uint8_t language = 0;
    if (!readUint32(input, record.fileId) || !readUint32(input, pathLength) ||
        !readString(input, pathLength, record.path) ||
        !input.read(reinterpret_cast<char*>(record.hash.data()),
                    static_cast<std::streamsize>(record.hash.size())) ||
        !readUint8(input, language)) {
      error = "Failed reading files.tbl record";
      return false;
    }
    record.language = static_cast<Language>(language);

    // For schema v1, files.tbl contained lastModified; for v2+ lastModified is in files.meta
    if (expectedSchemaVersion <= 1U) {
      std::uint64_t lastMod = 0;
      if (!readUint64(input, lastMod)) {
        error = "Failed reading files.tbl record (lastModified)";
        return false;
      }
      record.lastModified = lastMod;
    } else {
      record.lastModified = 0;
    }

    outFiles.push_back(std::move(record));
  }

  // If schema version is v2 or newer, attempt to read companion files.meta for timestamps
  if (expectedSchemaVersion >= 2U) {
    const std::filesystem::path metaPath = tablePath.parent_path() / "files.meta";
    if (std::filesystem::exists(metaPath)) {
      std::ifstream metaIn(metaPath, std::ios::binary);
      if (!metaIn) {
        error = "Failed to open files.meta: " + metaPath.string();
        return false;
      }
      std::map<std::uint32_t, std::uint64_t> metaMap;
      while (true) {
        std::uint32_t fid = 0;
        if (!readUint32(metaIn, fid)) {
          if (metaIn.eof()) break;
          error = "Failed reading files.meta file_id";
          return false;
        }
        std::uint64_t lm = 0;
        if (!readUint64(metaIn, lm)) {
          error = "Failed reading files.meta lastModified";
          return false;
        }
        metaMap[fid] = lm;
      }
      for (FileRecord& rec : outFiles) {
        auto it = metaMap.find(rec.fileId);
        if (it != metaMap.end()) rec.lastModified = it->second;
      }
    }
  }

  return true;
}

bool BinaryIndexReader::readSymbolsTable(
    const std::filesystem::path& tablePath,
    const std::uint32_t expectedSchemaVersion,
    std::vector<SymbolRecord>& outSymbols,
    std::string& error) {
  outSymbols.clear();
  std::ifstream input(tablePath, std::ios::binary);
  if (!input) {
    error = "Failed to open symbols.tbl: " + tablePath.string();
    return false;
  }

  std::uint32_t recordCount = 0;
  if (!readTableHeader(input, BinaryIndexWriter::kSymbolsMagic, expectedSchemaVersion,
                       recordCount, error)) {
    return false;
  }

  outSymbols.reserve(recordCount);
  for (std::uint32_t i = 0; i < recordCount; ++i) {
    SymbolRecord record;
    std::uint32_t nameLength = 0;
    std::uint8_t symbolType = 0;
    std::uint8_t visibility = 0;
    if (!readUint64(input, record.symbolId) || !readUint32(input, record.fileId) ||
        !readUint32(input, nameLength) ||
        !readString(input, nameLength, record.name) ||
        !readUint8(input, symbolType) || !readUint8(input, visibility) ||
        !readUint32(input, record.lineNumber)) {
      error = "Failed reading symbols.tbl record";
      return false;
    }
    record.symbolType = static_cast<SymbolType>(symbolType);
    record.visibility = static_cast<Visibility>(visibility);
    outSymbols.push_back(std::move(record));
  }

  return true;
}

bool BinaryIndexReader::readDependenciesTable(
    const std::filesystem::path& tablePath,
    const std::uint32_t expectedSchemaVersion,
    DependencyTableData& outDeps,
    std::string& error) {
  outDeps.fileEdges.clear();
  outDeps.symbolEdges.clear();

  std::ifstream input(tablePath, std::ios::binary);
  if (!input) {
    error = "Failed to open deps.tbl: " + tablePath.string();
    return false;
  }

  std::uint32_t recordCount = 0;
  if (!readTableHeader(input, BinaryIndexWriter::kDepsMagic, expectedSchemaVersion,
                       recordCount, error)) {
    return false;
  }

  for (std::uint32_t i = 0; i < recordCount; ++i) {
    std::uint8_t edgeType = 0;
    std::uint64_t fromId = 0;
    std::uint64_t toId = 0;
    if (!readUint8(input, edgeType) || !readUint64(input, fromId) ||
        !readUint64(input, toId)) {
      error = "Failed reading deps.tbl record";
      return false;
    }
    if (edgeType == static_cast<std::uint8_t>(DependencyEdgeType::File)) {
      FileDependencyEdge edge;
      edge.fromFileId = static_cast<std::uint32_t>(fromId);
      edge.toFileId = static_cast<std::uint32_t>(toId);
      outDeps.fileEdges.push_back(edge);
    } else if (edgeType ==
               static_cast<std::uint8_t>(DependencyEdgeType::Symbol)) {
      SymbolDependencyEdge edge;
      edge.fromSymbolId = fromId;
      edge.toSymbolId = toId;
      outDeps.symbolEdges.push_back(edge);
    } else {
      error = "Unknown edge type in deps.tbl";
      return false;
    }
  }

  return true;
}

}  // namespace ultra::ai

