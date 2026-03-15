#pragma once

#include "Hashing.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ultra::ai {

enum class Language : std::uint8_t {
  Unknown = 0,
  Cpp = 1,
  JavaScript = 2,
  TypeScript = 3,
  Python = 4,
  Java = 5,
  Go = 6,
  Rust = 7,
  CSharp = 8
};

struct DiscoveredFile {
  std::uint32_t fileId{0};
  std::filesystem::path absolutePath;
  std::string relativePath;
  Language language{Language::Unknown};
  std::uint64_t lastModified{0};
};

struct FileRecord {
  FileRecord() = default;
  FileRecord(std::string pathValue, Sha256Hash hashValue)
      : path(std::move(pathValue)), hash(hashValue) {}

  std::uint32_t fileId{0};
  std::string path;
  Sha256Hash hash{};
  Language language{Language::Unknown};
  std::uint64_t lastModified{0};
};

class FileRegistry {
 public:
  static std::vector<DiscoveredFile> discoverProjectFiles(
      const std::filesystem::path& projectRoot);

  static std::vector<FileRecord> deriveRecords(
      const std::vector<DiscoveredFile>& discoveredFiles);

  static std::map<std::string, FileRecord> mapByPath(
      const std::vector<FileRecord>& records);

  static std::map<std::uint32_t, std::string> mapPathById(
      const std::vector<FileRecord>& records);

  static Language detectLanguage(const std::filesystem::path& path);

  static std::string languageToString(Language language);

  static std::string toRelativeUtf8Path(const std::filesystem::path& root,
                                        const std::filesystem::path& absolute);

  static std::uint64_t fileTimeToUint64(std::filesystem::file_time_type value);
};

}  // namespace ultra::ai